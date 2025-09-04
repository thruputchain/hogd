#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "db_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/metrics.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/init.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "loadgen.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {

namespace {

struct BenchmarkState {

  BenchmarkState(uint32_t num_accounts) : num_accounts_(num_accounts) {}

  std::atomic<TransactionID> next_tx_id_ = 0;
  BlockID block_number_ = 0;

  const uint32_t num_accounts_;

  x23::ThreadPool db_pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(db_pool_);

  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_;

  using coord_client_t =
      DbCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  std::unique_ptr<coord_client_t> coord_client_;

  // returns the work transactions
  x23::Vec<SignedTransaction> initialize(uint32_t exec_port,
                                         uint32_t coord_port, uint32_t num_txs)
  {
    HogexecMetrics::launch_log_worker();

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

    auto exp = ERC20ExperimentState(ERC20PaymentsConfig{
        .num_accounts = num_accounts_,
    });
    auto result = exp.generate_experiment();
    if (!result.has_value()) {
      throw std::runtime_error("Failed to generate erc20 payments experiment");
    }

    {
      auto r = coord_client_->dbcoord_load_genesis_block(
          result.value().genesis_block)();
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

    for (auto const &work : result.value().initialization_txs) {
      for (auto const &tx : work) {
        auto handler = make_handler(tx);
        if (!handler.root_invoke().is_root_success()) {
          throw std::runtime_error("Failed to invoke transaction");
        }
        if (auto r = handler.try_commit_transaction();
            !r.has_value() || !r.value()) {
          throw std::runtime_error("failed to perform commit operation");
        }
      }
      advance_block();
    }

    auto work_txs = exp.extend_experiment(num_txs);
    if (!work_txs.has_value()) {
      throw std::runtime_error("Failed to extend experiment");
    }

    return work_txs.value().work_txs;
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
run_bench_rep(BenchmarkState &bs, std::span<SignedTransaction> total_work_txs,
              int num_exec_threads, uint32_t num_txs_per_rep, size_t num_reps)
{
  WorkerPool exec_pool(num_exec_threads);
  exec_pool.adjust_workers(num_exec_threads);
  assert((total_work_txs.size() % num_reps) == 0);
  assert(num_txs_per_rep == total_work_txs.size() / num_reps);

  for (size_t nrep = 0U; nrep < num_reps; nrep++) {
    info("bench rep {} for {} txs (accts {}) on {} threads", nrep,
         num_txs_per_rep, bs.num_accounts_, num_exec_threads);
    auto work_txs =
        total_work_txs.subspan(nrep * num_txs_per_rep, num_txs_per_rep);

    auto start_time = std::chrono::steady_clock::now();

    uint32_t ntx_per_thread = num_txs_per_rep / num_exec_threads;
    if (num_txs_per_rep % num_exec_threads != 0) {
      ntx_per_thread++;
    }

    std::vector<std::future<void>> futures;
    futures.reserve(num_exec_threads);

    for (int i = 0; i < num_exec_threads; i++) {
      int offset = i * ntx_per_thread;
      int cur_count = std::min(ntx_per_thread, num_txs_per_rep - (offset));

      futures.emplace_back(
          exec_pool.submit([&bs, work_txs, offset, cur_count]() -> void {
            for (int j = 0; j < cur_count; j++) {

              auto const &tx = work_txs[offset + j];

              auto handler = bs.make_handler(tx);
              if (!handler.root_invoke().is_root_success()) {
                error("Failed to invoke transaction");
                throw std::runtime_error("Failed to invoke transaction");
              }
              if (auto r = handler.try_commit_transaction();
                  !r.has_value() || !r.value()) {
                throw std::runtime_error("failed to perform commit operation");
              }
            }
          }));
    }
    for (auto &f : futures) {
      f.wait();
    }
    auto finish_time = std::chrono::steady_clock::now();
    std::println("time taken: {} us for {} txs (accts {}) on {} threads",
                 std::chrono::duration_cast<std::chrono::microseconds>(
                     finish_time - start_time)
                     .count(),
                 num_txs_per_rep, bs.num_accounts_, num_exec_threads);
    bs.advance_block();
  }
}

void
run_payments_bench(uint32_t exec_port, uint32_t coord_port,
                   uint32_t num_accounts)
{
  // This is wayyy to slow if we generate and sign all the txs up front.
  // For now, we just reuse the transactions from one rep to the next.
  uint32_t num_total_txs = (51200 * 10);
  BenchmarkState bs(num_accounts);
  auto work_txs = bs.initialize(exec_port, coord_port, num_total_txs);
  size_t offset = 0;

  // strong scaling
  for (auto nthread : {1, 4, 16, 64, 128, 256, 512, 2048}) {
    assert(offset + 10000 * 10 <= work_txs.size());
    run_bench_rep(bs,
                  std::span<SignedTransaction>(work_txs).subspan(offset,
                                                                 10000 * 10),
                  nthread, 10000, 10);
    // offset += 10000 * 10;
  }
  // weak scaling
  for (auto nthread : {1, 4, 16, 32, 64, 128, 256, 512}) {
    assert(offset + 10000 * 10 <= work_txs.size());
    run_bench_rep(bs,
                  std::span<SignedTransaction>(work_txs).subspan(offset,
                                                                 10000 * 10),
                  nthread, 100 * nthread, 10);
    // offset += 10000 * 10;
  }
}
} // namespace

} // namespace hogd

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} -e <exec_port> -c <coord_port> -a <num_accounts>",
               argv[0]);
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
  std::optional<int> num_accounts;
  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "e:c:a:")) != -1) {
    switch (opt) {
    case 'e':
      exec_port = std::stoi(optarg);
      break;
    case 'c':
      coord_port = std::stoi(optarg);
      break;
    case 'a':
      num_accounts = std::stoi(optarg);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (!exec_port || !coord_port || !num_accounts) {
    usage(argv);
  }

  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  hogd::run_payments_bench(*exec_port, *coord_port, *num_accounts);
  std::fflush(stdout);
  return 0;
}
