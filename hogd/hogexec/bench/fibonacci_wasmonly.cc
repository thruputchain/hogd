#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "db_coord.hh"
#include "hogd/base/io.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_coord.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "test_contracts/integration_test_contracts/fibonacci/fibonacci.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/serialize.h"
#include "x23/threadpool.h"

namespace hogd {

namespace {

struct BenchmarkState {

  BenchmarkState(x23::ThreadPool &db_pool) : db_pool_(db_pool) {}

  std::atomic<TransactionID> next_tx_id_ = 0;
  BlockID block_number_ = 0;

  x23::ThreadPool &db_pool_;
  x23::CoroPoll cp_ = x23::CoroPoll(db_pool_);

  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_;

  using coord_client_t =
      DbCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  std::unique_ptr<coord_client_t> coord_client_;

  void initialize(uint32_t exec_port, uint32_t coord_port)
  {
    db_proxy_ = std::make_unique<DatabaseProxy>(
        LocalMockDB{.read_port_number_ = exec_port});

    coord_client_ = [&] -> std::unique_ptr<coord_client_t> {
      auto fd = cp_.tcpconnect("localhost", std::to_string(coord_port))();
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

  SystemCallHandler make_handler(SignedTransaction const &signed_tx,
                                 TransactionID txid = 0)
  {
    if (txid == 0) {
      txid = allocate_txid();
    }
    auto proxy = PerTransactionDatabaseProxy(*db_proxy_, block_number_, txid);
    return SystemCallHandler(signed_tx, *snapshot_cache_, proxy,
                             ProposalModeData{});
  }

  void advance_block()
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

  TransactionID allocate_txid()
  {
    return next_tx_id_.fetch_add(1, std::memory_order_relaxed);
  }
};

void
setup_grant_storage_token(HogDatabase &db,
                   
                          ContractAddress const &address, int128_t amount)
{
  assert(amount >= 0);
  assert(amount <= int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

  // Convert ContractAddress to Address - both are std::array<uint8_t, 32>
  Address addr(address);
  auto ret = write_address_balance(db, addr, amount);
  if (!ret) {
    throw std::runtime_error("Failed to grant storage token");
  }
}

void
setup_benchmark(BenchmarkState &bs, uint32_t exec_port, uint32_t coord_port)
{
  bs.initialize(exec_port, coord_port);
}

void
run_gas_correlation_experiment(BenchmarkState &bs, int num_txs, uint32_t nfib)
{
  WorkerPool exec_pool(1);
  exec_pool.adjust_workers(1);

  using fib_client_t = ContractTraitClient<Fibonacci>;
  fib_client_t fib_client(ContractAddress{});
  std::optional<uint64_t> gas_consumed;

  for (size_t nrep = 0; nrep < 10; nrep++) {

    auto start_time = std::chrono::steady_clock::now();

    for (int i = 0; i < num_txs; i++) {
      auto f = exec_pool.submit([&bs, &fib_client, &nfib]() -> uint64_t {
        auto tx = SignedTransaction{
            .tx = Transaction{
                .invocation =
                    fib_client.make_invocation<"slow_fibonacci">(UINT64_MAX,
                                                                 nfib)}};
        auto handler = bs.make_handler(tx);
        auto r = handler.root_invoke();
        if (!r.is_root_success()) {
          throw std::runtime_error("Failed to invoke transaction");
        }
        return r.gas_consumed_;
      });
      auto g = f.get();
      if (!gas_consumed) {
        gas_consumed = g;
      }
      else {
        assert(gas_consumed.value() == g);
      }
    }
    assert(gas_consumed.has_value());
    auto finish_time = std::chrono::steady_clock::now();
    std::println(
        "time taken: {} ms for {} txs (fib {}), gas consumed: {}",
        std::chrono::duration_cast<std::chrono::milliseconds>(finish_time -
                                                              start_time)
            .count(),
        num_txs, nfib,
        gas_consumed.value()); // NOLINT(bugprone-unchecked-optional-access)
  }
}

void
run_bench_rep(BenchmarkState &bs, int num_txs, uint32_t nfib,
              int num_exec_threads, bool do_commit)
{
  WorkerPool exec_pool(num_exec_threads);
  exec_pool.adjust_workers(num_exec_threads);

  using fib_client_t = ContractTraitClient<Fibonacci>;
  fib_client_t fib_client(ContractAddress{});

  for (int nrep = 0; nrep < 10; nrep++) {

    auto start_time = std::chrono::steady_clock::now();

    int ntx_per_thread = num_txs / num_exec_threads;
    if (num_txs % num_exec_threads != 0) {
      ntx_per_thread++;
    }

    std::vector<std::future<void>> futures;
    futures.reserve(num_exec_threads);
    const auto tx = SignedTransaction{
        .tx = Transaction{
            .invocation =
                fib_client.make_invocation<"slow_fibonacci">(UINT64_MAX,
                                                             nfib)}};

    for (int i = 0; i < num_exec_threads; i++) {
      int offset = i * ntx_per_thread;
      int cur_count = std::min(ntx_per_thread, num_txs - (offset));

      futures.emplace_back(exec_pool.submit([&bs, &tx, do_commit, offset,
                                             cur_count]() -> void {
        for (int j = 0; j < cur_count; j++) {

          auto handler = bs.make_handler(tx, offset + j);
          if (!handler.root_invoke().is_root_success()) {
            throw std::runtime_error("Failed to invoke transaction");
          }
          if (do_commit) {
            if (auto r = handler.try_commit_transaction();
                !r.has_value() || !r.value()) {
              throw std::runtime_error("failed to perform commit operation");
            }
          }
        }
      }));
    }
    for (auto &f : futures) {
      f.wait();
    }
    auto finish_time = std::chrono::steady_clock::now();
    std::println(
        "time taken: {} us for {} txs (fib {}, commit {}) on {} threads",
        std::chrono::duration_cast<std::chrono::microseconds>(finish_time -
                                                              start_time)
            .count(),
        num_txs, nfib, do_commit, num_exec_threads);
  }
}

void
run_fibonacci_server(int nthreads, uint32_t exec_port, uint32_t coord_port)
{
  x23::ThreadPool db_pool = x23::ThreadPool(nthreads);

  // Create a temporary directory for the database
  Directory dir = Directory::open_temporary().value();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;
  
  // Open the database
  auto db_result = Database::open(dir.get_path(), kDefaultHeapSize);
  if (!db_result) {
    throw std::runtime_error("Failed to open database");
  }
  
  HogDatabase db(std::move(*db_result));
  MerkleTriePrefix trie;
  RPCContext ctx(db, trie);

  //StorageCostModel cost_model = StorageCostModel(12000, 1, 5, 100, 5);
  ContractAddress contract_addr{};

  auto contract = test::load_test_contract("fibonacci.wasm");

  // Write contract to hogdb
  auto bytes = x23::serialize(*contract);
  auto contract_hash = sha256(*contract);
  auto ret = write_wasm_code(db, contract_hash, std::as_bytes(std::span(bytes)));
  if (!ret) {
    throw std::runtime_error("Failed to write contract");
  }

  // Write deployment
  ret = write_address_wasm_hash(db, contract_addr, contract_hash);
  if (!ret) {
    throw std::runtime_error("Failed to write deployment");
  }

  setup_grant_storage_token(db, contract_addr, 1'000'000'000);

  // Start the coordinator server
  std::println("Starting coordinator server on port {}", coord_port);
  HogDBCoordServer db_coord_server(ctx);
  db_coord_server.start_server(coord_port);

  // Start the executor server
  std::println("Starting executor server on port {}", exec_port);
  HogDBExecutorServer db_executor_server(ctx);
  auto exec_ret = db_executor_server.start_server(exec_port);
  if (!exec_ret) {
    throw std::runtime_error("Failed to start executor server");
  }

  // Wait for a byte from stdin (blocking)
  char buf;
  if (read(STDIN_FILENO, &buf, 1) > 0) {
    std::println("Exiting server on stdin input");
  }
}

void
run_fibonacci_client(uint32_t exec_port, uint32_t coord_port)
{
  x23::ThreadPool db_pool = x23::ThreadPool(1);
  BenchmarkState bs(db_pool);

  setup_benchmark(bs, exec_port, coord_port);

  // strong scaling
  for (auto nthread : {1, 4, 16, 64, 128, 256, 512, 2048}) {
    run_bench_rep(bs, 10000, 20, nthread, true);
    run_bench_rep(bs, 10000, 20, nthread, false);
    run_bench_rep(bs, 10000, 15, nthread, true);
    run_bench_rep(bs, 10000, 15, nthread, false);
  }
  // weak scaling
  for (auto nthread : {1, 4, 16, 32, 64, 128, 256, 512}) {
    run_bench_rep(bs, nthread * 1000, 20, nthread, true);
    run_bench_rep(bs, nthread * 1000, 20, nthread, false);
    run_bench_rep(bs, nthread * 1000, 15, nthread, true);
    run_bench_rep(bs, nthread * 1000, 15, nthread, false);
  }

  // measure per tx time
  for (auto nfib : {
           0,
           1,
           2,
           5,
           8,
           10,
           12,
           15,
           18,
           20,
           22,
           25,
           28,
           30,
       }) {
    run_gas_correlation_experiment(bs, 50, nfib);
  }
}
} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  int exec_port = 10000;
  int coord_port = 10001;

  bool run_client = false;
  bool run_server = false;

  int opt;
  // NOLINTNEXTLINE(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "cs")) != -1) {
    switch (opt) {
    case 'c':
      run_client = true;
      break;
    case 's':
      run_server = true;
      break;
    default:
      std::println("Usage: {} [-c] [-s]", argv[0]);
      return 1;
    }
  }

  if (run_client == run_server) {
    std::println(
        "Error: Must specify exactly one of -c (client) or -s (server)");
    return 1;
  }

  if (run_server) {
    hogd::run_fibonacci_server(1, exec_port, coord_port);
  }
  else if (run_client) {
    hogd::run_fibonacci_client(exec_port, coord_port);
  }

  return 0;
}
