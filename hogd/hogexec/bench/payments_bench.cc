#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/bench/common_bench_state.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/metrics.h"
#include "hogd/init.h"
#include "hogd/loadgen/erc20_experiment.h"
#include "loadgen.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

namespace {

struct BenchmarkState : public CommonBenchState {
  BenchmarkState(std::vector<std::string> server_addrs, uint16_t exec_port,
                 uint16_t coord_port, uint32_t num_accounts, bool use_rv32)
    : CommonBenchState(std::move(server_addrs), exec_port, coord_port),
      num_accounts_(num_accounts), use_rv32_(use_rv32)
  {}

  const uint32_t num_accounts_;
  const bool use_rv32_;

  // TODO(geoff): This benchmark will break down when we start tracking
  // uniquifiers within hogdb.  We need to somehow clean up the benchmark state
  // so that uniquifiers don't grow without bound. The best path forward might
  // be to call exp.extend_experiment() once per bench iteration or per
  // benchmark, and set the expiry time to be that reps top block height.

  // returns the work transactions
  x23::XdrVec<SignedTransaction> initialize(uint32_t num_txs)
  {
    HogexecMetrics::launch_log_worker();

    auto exp = ERC20ExperimentState(
        ERC20PaymentsConfig{
            .num_addresses = num_accounts_,
        },
        use_rv32_);
    auto result = exp.generate_experiment()();
    if (!result.has_value()) {
      throw std::runtime_error("Failed to generate erc20 payments experiment");
    }

    auto genesis_configuration = result.value().genesis_configuration;
    run_genesis_configuration(genesis_configuration,
                              result.value().initialization_txs);

    auto work_txs_raw = exp.extend_experiment(num_txs, 1'000'000)();
    if (!work_txs_raw.has_value()) {
      throw std::runtime_error("Failed to extend experiment");
    }

    x23::XdrVec<SignedTransaction> work_txs;
    parallel_batch_to_t(work_txs_raw.value().work_txs, work_txs)();
    return work_txs;
  }
};

void
run_bench_rep(BenchmarkState &bs, std::span<SignedTransaction> total_work_txs,
              int num_exec_threads, uint32_t num_txs_per_rep, size_t num_reps)
{
  WorkerPool exec_pool(num_exec_threads);
  exec_pool.adjust_workers(num_exec_threads);
  if (total_work_txs.size() < num_txs_per_rep * num_reps) {
    throw std::runtime_error("not enough transactions to run the benchmark");
  }
  assert((total_work_txs.size() % num_reps) == 0);
  assert(num_txs_per_rep == total_work_txs.size() / num_reps);

  for (size_t nrep = 0U; nrep < num_reps; nrep++) {
    info("bench rep {} for {} txs (accts {}) on {} threads", nrep,
         num_txs_per_rep, bs.num_accounts_, num_exec_threads);
    auto work_txs =
        total_work_txs.subspan(nrep * num_txs_per_rep, num_txs_per_rep);

    // The work txs are reused across reps and scaling configs, so overwrite
    // their uniquifiers each rep (the signature covers only the inner calldata,
    // not the uniquifier). Capture the height they commit against so expiry
    // heights are not yet expired; cleared after the rep.
    const auto base_height = bs.block_height();
    auto start_time = std::chrono::steady_clock::now();

    uint32_t ntx_per_thread = num_txs_per_rep / num_exec_threads;
    if (num_txs_per_rep % num_exec_threads != 0) {
      ntx_per_thread++;
    }

    std::vector<std::future<void>> futures;
    futures.reserve(num_exec_threads);

    std::atomic<uint64_t> pending_events = num_txs_per_rep;
    std::atomic<bool> events_done = false;

    for (int i = 0; i < num_exec_threads; i++) {
      uint32_t offset = static_cast<uint32_t>(i) * ntx_per_thread;
      if (offset >= num_txs_per_rep) {
        break;
      }
      uint32_t cur_count = std::min(ntx_per_thread, num_txs_per_rep - offset);

      futures.emplace_back(exec_pool.submit([&bs, work_txs, offset, cur_count,
                                             base_height, &pending_events,
                                             &events_done]() -> void {
        auto runtime_context = make_hogexec_context();

        for (uint32_t j = 0; j < cur_count; j++) {

          uint32_t k = offset + j;
          work_txs[k].tx.uniquifier =
              CommonBenchState::bench_uniquifier(base_height, k);
          auto const &tx = work_txs[k];

          auto handler =
              bs.make_handler(tx, bs.allocate_txid(), *runtime_context);
          if (!handler.root_invoke().is_root_success()) {
            error("Failed to invoke transaction");
            throw std::runtime_error("Failed to invoke transaction");
          }
          if (handler.try_commit_transaction() != ExecResult::ExecResultOK) {
            throw std::runtime_error("failed to perform commit operation");
          }
          handler.emit_events(ExecResult::ExecResultOK, ProposedTransaction{})(
              [&pending_events,
               &events_done](this auto,
                             SystemStatus<void> res) -> x23::Detached {
                if (!res.has_value()) {
                  panic("failed to emit events");
                }
                auto r = pending_events.fetch_sub(1, std::memory_order_relaxed);
                if (r == 1) {
                  events_done.store(true, std::memory_order_relaxed);
                  events_done.notify_one();
                }
                co_return;
              });
        }
      }));
    }
    for (auto &f : futures) {
      f.wait();
    }
    events_done.wait(false);
    auto finish_time = std::chrono::steady_clock::now();
    std::println("time taken: {} us for {} txs (accts {}) on {} threads",
                 std::chrono::duration_cast<std::chrono::microseconds>(
                     finish_time - start_time)
                     .count(),
                 num_txs_per_rep, bs.num_accounts_, num_exec_threads);
    info("bench rep tps: {}",
         num_txs_per_rep * 1'000'000.0 /
             std::chrono::duration_cast<std::chrono::microseconds>(finish_time -
                                                                   start_time)
                 .count());
    // Expire this rep's uniquifiers so the reused txs can be re-committed with
    // fresh uniquifiers next rep/config. Outside the timed region.
    bs.clear_uniquifiers(num_txs_per_rep);
  }
}

void
run_payments_bench(std::vector<std::string> const &server_addrs,
                   uint32_t exec_port, uint32_t coord_port,
                   uint32_t num_accounts, bool use_rv32)
{
  info("payments bench using {} contracts", use_rv32 ? "rv32" : "wasm");
  // This is wayyy to slow if we generate and sign all the txs up front.
  // For now, we just reuse the transactions from one rep to the next.
  size_t num_reps = 10;
  uint32_t num_total_txs =
      std::max<uint32_t>(100000 * num_reps, 204800 * num_reps);
  BenchmarkState bs(server_addrs, exec_port, coord_port, num_accounts,
                    use_rv32);
  auto work_txs = bs.initialize(num_total_txs);
  size_t offset = 0;

  // strong scaling
  for (auto nthread : {64, 128, 256, 512, 1024, 2048}) {
    assert(offset + (100000 * num_reps) <= work_txs.size());
    run_bench_rep(bs,
                  std::span<SignedTransaction>(work_txs).subspan(offset,
                                                                 100000 *
                                                                     num_reps),
                  nthread, 100000, num_reps);
  }
  // weak scaling
  for (auto nthread : {64, 128, 256, 512, 1024, 2048}) {
    assert(offset + (100 * nthread * num_reps) <= work_txs.size());
    run_bench_rep(bs,
                  std::span<SignedTransaction>(work_txs).subspan(offset,
                                                                 100 * nthread *
                                                                     num_reps),
                  nthread, 100 * nthread, num_reps);
  }
}
} // namespace

} // namespace hogd

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} -e <exec_port> -c <coord_port> -s <server_addr, "
               "repeated (2 required)> -a "
               "<num_accounts> [-r (use rv32 contracts)]",
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
  std::vector<std::string> server_addrs;
  std::optional<int> num_accounts;
  bool use_rv32 = false;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "e:c:s:a:r")) != -1) {
    switch (opt) {
    case 'e':
      exec_port = std::stoi(optarg);
      break;
    case 'c':
      coord_port = std::stoi(optarg);
      break;
    case 's':
      server_addrs.emplace_back(optarg);
      break;
    case 'a':
      num_accounts = std::stoi(optarg);
      break;
    case 'r':
      use_rv32 = true;
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (server_addrs.size() != 2 || !exec_port || !coord_port || !num_accounts) {
    usage(argv);
  }

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  hogd::run_payments_bench(server_addrs, *exec_port, *coord_port, *num_accounts,
                           use_rv32);
  // NOLINTEND(bugprone-unchecked-optional-access)

  std::fflush(stdout);
  return 0;
}
