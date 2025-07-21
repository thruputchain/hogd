#include "hogd/mempool/mempool.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "db_coord.hh"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/db_coord_server.h"
#include "hogd/db_interface/db_executor_server.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/init.h"
#include "hogd/mockdb/in_memory_database.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "test_contracts/integration_test_contracts/fibonacci/fibonacci.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

namespace {

void
setup_grant_storage_token(InMemoryDatabase &in_memory_db,
                          StorageCostModel const &cost_model,
                          ContractAddress const &address, int128_t amount)
{
  CHECK_GE(amount, 0);
  CHECK_LE(amount, int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

  AddressAndKey addrkey = {
      .address = address,
      .key = StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};

  in_memory_db.write_item(
      addrkey, std::make_shared<const StorageItemWrapper>(amount, &cost_model));
}

} // namespace

Validator::Validator(size_t num_threads)
  : db_pool_(x23::ThreadPool(num_threads)), cost_model_(12000, 1, 5, 100, 5),
    block_manager_(0, &in_memory_db_),
    db_executor_server_(&block_manager_, kExecutorPort),
    db_coordinator_server_(&block_manager_)
{
  ContractAddress contract_addr;
  std::shared_ptr<Contract> contract =
      test::load_test_contract(kTestContractFileName);

  in_memory_db_.write_contract(sha256(*contract), contract);
  in_memory_db_.write_deployment(contract_addr, sha256(*contract));

  setup_grant_storage_token(in_memory_db_, cost_model_, contract_addr,
                            1'000'000'000);

  std::println("Starting executor server on port {}", kExecutorPort);
  std::println("Starting coord server on port {}", kCoordinatorPort);
  db_coordinator_server_.start_server(kCoordinatorPort);

  // Initialize the client.
  db_proxy_ = std::make_unique<DatabaseProxy>(
      LocalMockDB{.read_port_number_ = kExecutorPort});

  coord_client_ = [&] -> std::unique_ptr<coord_client_t> {
    auto fd = cp_.tcpconnect("localhost", std::to_string(kCoordinatorPort))();
    if (!fd) {
      throw std::runtime_error("Failed to connect to coord server");
    }
    auto client_config = x23::rpc_stream_logged(cp_, *fd);
    return std::make_unique<coord_client_t>(
        client_config.template client<DbCoord1>());
  }();

  auto r = coord_client_->dbcoord_create_snapshot(
      0, 1, BlockExecutionMode::kBlockExecutionModeProposal)();
  if (r != DbResult::DbResultOK) {
    throw std::runtime_error("Failed to create snapshot");
  }

  block_number_++;

  snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);
}

SystemCallHandler
Validator::make_handler(SignedTransaction const &signed_tx, TransactionID txid)
{
  auto proxy = PerTransactionDatabaseProxy(*db_proxy_, block_number_, txid);
  return SystemCallHandler(signed_tx, *snapshot_cache_, proxy,
                           ProposalModeData{});
}

void
Validator::advance_block()
{
  auto r = coord_client_->dbcoord_close_snapshot(block_number_)();
  if (r.result() != DbResult::DbResultOK) {
    throw std::runtime_error("Failed to close snapshot");
  }

  auto r2 = coord_client_->dbcoord_commit_snapshot(block_number_)();
  if (r2.result() != DbResult::DbResultOK) {
    throw std::runtime_error("Failed to commit snapshot");
  }

  auto r3 = coord_client_->dbcoord_create_snapshot(
      block_number_, block_number_ + 1,
      BlockExecutionMode::kBlockExecutionModeProposal)();
  if (r3 != DbResult::DbResultOK) {
    throw std::runtime_error("Failed to create snapshot");
  }

  block_number_++;

  snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);
}

TransactionID
Validator::allocate_txid()
{
  return next_tx_id_.fetch_add(1, std::memory_order_relaxed);
}

void
Validator::run_bench_rep(int num_txs, uint32_t nfib, int num_exec_threads,
                         MerkleTriePrefix &trie)
{
  WorkerPool exec_pool(num_exec_threads);
  exec_pool.adjust_workers(num_exec_threads);

  using fib_client_t = ContractTraitClient<Fibonacci>;
  fib_client_t fib_client(ContractAddress{});

  auto start_time = std::chrono::steady_clock::now();

  int ntx_per_thread = num_txs / num_exec_threads;
  if (num_txs % num_exec_threads != 0) {
    ntx_per_thread++;
  }

  std::vector<std::future<void>> futures;
  futures.reserve(num_exec_threads);
  const SignedTransaction tx{
      .tx = Transaction{
          .invocation =
              fib_client.make_invocation<"slow_fibonacci">(UINT64_MAX, nfib)}};

  for (int i = 0; i < num_exec_threads; i++) {
    const int offset = i * ntx_per_thread;
    int cur_count = std::min(ntx_per_thread, num_txs - offset);

    futures.emplace_back(
        exec_pool.submit([this, &trie, &tx, offset, cur_count]() -> void {
          for (int j = 0; j < cur_count; j++) {
            TransactionID txid = offset + j;
            auto handler = make_handler(tx, txid);
            if (!handler.root_invoke().is_root_success()) {
              throw std::runtime_error("Failed to invoke transaction");
            }
            if (auto r = handler.try_commit_transaction();
                !r.has_value() || !r.value()) {
              throw std::runtime_error("failed to perform commit operation");
            }
            if (!trie.insert(HexArray(txid), std::span<const std::byte>())) {
              throw std::runtime_error("could not insert tx into trie");
            }
          }
        }));
  }
  for (auto &f : futures) {
    f.wait();
  }
  auto finish_time = std::chrono::steady_clock::now();
  std::println("time taken: {} us for {} txs (fib {}) on {} threads",
               std::chrono::duration_cast<std::chrono::microseconds>(
                   finish_time - start_time)
                   .count(),
               num_txs, nfib, num_exec_threads);
}

bool
Validator::validate(const SHA256Hash &expected_root_hash,
                    const std::vector<SignedTransaction> &txs)
{
  // TODO(jhumphri):
  // 1. Send the txs to the executors and get the results.
  // 2. Throw out any txs that fail?
  // 3. Build a Merkle Trie of the txs.
  // 4. Compare the hash of the Merkle Trie root with [expected_root_hash].
  //
  // Follow up:
  // 1. We can probably build the Merkle Trie while the executors are executing
  //    the txs. If a tx fails, we can remove it from the Merkle Trie. This is
  //    likely more efficient than building the Merkle Trie only after we know
  //    which txs succeeded.
  // 2. Experiment with different scheduling policies for txs. We want to model
  //    txs of different lengths and multiple blocks validated at the same time.
  MerkleTriePrefix trie;
  run_bench_rep(/*num_txs=*/10, /*nfib=*/10, /*num_exec_threads=*/64, trie);

  trie.generate_top_level_hashes();
  return trie.get_root_hash() == expected_root_hash;
}

} // namespace hogd
