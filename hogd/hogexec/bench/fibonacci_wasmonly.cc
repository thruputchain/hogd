#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci.h"
#include "db_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_block.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/init.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/rpc.h"
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

  ContractAddress contract_addr_;

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

    GenesisBlockBuilder genesis_builder;
    auto fibonacci_contract = load_contract("fibonacci.wasm");
    using client_meta_t = ContractTraitMeta<fibonacci::Fibonacci>;
    genesis_builder.add_initial_config(
        fibonacci_contract, SHA256Hash{},
        client_meta_t::entrypoint<"fibonacci">(),
        client_meta_t::make_calldata<"fibonacci">(uint32_t{5}), 1'000'000'000);
    auto genesis_block = genesis_builder.build();

    {
      auto r = coord_client_->dbcoord_load_genesis_block(genesis_block)();
      if (r != DbResult::DbResultOK) {
        throw std::runtime_error("Failed to write genesis block");
      }
    }

    {
      auto r = coord_client_->dbcoord_create_snapshot(
          0, 1, BlockExecutionMode::kBlockExecutionModeProposal)();
      if (r != DbResult::DbResultOK) {
        throw std::runtime_error("Failed to create snapshot");
      }
    }

    block_number_++;

    snapshot_cache_ =
        std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);

    auto unfurl_txs = genesis_builder.make_unfurl_genesis_transactions();
    for (auto &tx : unfurl_txs) {
      auto handler = make_handler(tx);
      if (!handler.root_invoke().is_root_success()) {
        throw std::runtime_error("Failed to invoke transaction");
      }
      if (auto r = handler.try_commit_transaction();
          !r.has_value() || !r.value()) {
        throw std::runtime_error("failed to perform commit operation");
      }
    }
    contract_addr_ = GenesisBlockLoader::calculate_deployed_address(
        sha256(*fibonacci_contract), SHA256Hash{});
    advance_block();
  }

  SystemCallHandler make_handler(SignedTransaction const &signed_tx,
                                 TransactionID txid = 0)
  {
    if (txid == 0) {
      txid = allocate_txid();
    }
    auto proxy = PerTransactionDatabaseProxy(*db_proxy_, block_number_, txid);
    return SystemCallHandler(signed_tx, *snapshot_cache_, proxy,
                             ProposalModeData{}, make_hogexec_context());
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
setup_benchmark(BenchmarkState &bs, uint32_t exec_port, uint32_t coord_port)
{
  bs.initialize(exec_port, coord_port);
}

void
run_gas_correlation_experiment(BenchmarkState &bs, int num_txs, uint32_t nfib)
{
  WorkerPool exec_pool(1);
  exec_pool.adjust_workers(1);

  using fib_client_t = ContractTraitClient<fibonacci::Fibonacci>;
  fib_client_t fib_client(bs.contract_addr_);
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

  using fib_client_t = ContractTraitClient<fibonacci::Fibonacci>;
  fib_client_t fib_client(bs.contract_addr_);

  for (int nrep = 0; nrep < 10; nrep++) {
    info("bench rep {} for {} txs (fib {}, commit {}) on {} threads", nrep,
         num_txs, nfib, do_commit, num_exec_threads);
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
            error("Failed to invoke transaction");
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

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} -e <exec_port> -c <coord_port>", argv[0]);
  exit(EXIT_FAILURE);
}
} // namespace
int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  std::optional<int> exec_port;
  std::optional<int> coord_port;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "e:c:")) != -1) {
    switch (opt) {
    case 'e':
      exec_port = std::stoi(optarg);
      break;
    case 'c':
      coord_port = std::stoi(optarg);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (!exec_port || !coord_port) {
    usage(argv);
  }

  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  hogd::run_fibonacci_client(*exec_port, *coord_port);
  return 0;
}
