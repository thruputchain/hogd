#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <future>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/bench/common_bench_state.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/metrics.h"
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
                 uint16_t coord_port, uint32_t num_accounts)
    : CommonBenchState(std::move(server_addrs), exec_port, coord_port),
      num_accounts_(num_accounts)
  {}

  const uint32_t num_accounts_;

  // TODO(geoff): This benchmark will break down when we start tracking
  // uniquifiers within hog_db.  We need to somehow clean up the benchmark state
  // so that uniquifiers don't grow without bound. The best path forward might
  // be to call exp.extend_experiment() once per bench iteration or per
  // benchmark, and set the expiry time to be that reps top block height.

  // returns the work transactions
  x23::XdrVec<SignedTransaction> initialize(uint32_t num_txs)
  {
    HogexecMetrics::launch_log_worker();

    auto exp = ERC20ExperimentState(ERC20PaymentsConfig{
        .num_addresses = num_accounts_,
    });
    auto result = exp.generate_experiment()();
    if (!result.has_value()) {
      panic("Failed to generate erc20 payments experiment");
    }

    auto genesis_configuration = result.value().genesis_configuration;
    run_genesis_configuration(genesis_configuration,
                              result.value().genesis_ceremony_txs,
                              result.value().initialization_txs);

    auto work_txs_raw = exp.extend_experiment(num_txs, 1'000'000)();
    if (!work_txs_raw.has_value()) {
      panic("Failed to extend experiment");
    }

    x23::XdrVec<SignedTransaction> work_txs;
    parallel_batch_to_t(work_txs_raw.value().work_txs, work_txs,
                        kNoBatchSizeLimit)();
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
    panic("not enough transactions to run the benchmark");
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
        auto runtime_context = make_hog_exec_context();

        for (uint32_t j = 0; j < cur_count; j++) {

          uint32_t k = offset + j;
          work_txs[k].tx.uniquifier =
              CommonBenchState::bench_uniquifier(base_height, k);
          auto const &tx = work_txs[k];

          auto handler =
              bs.make_handler(tx, bs.allocate_txid(), *runtime_context);
          if (!handler.root_invoke()().is_root_success()) {
            error("Failed to invoke transaction");
            panic("Failed to invoke transaction");
          }
          if (handler.try_commit_transaction()() != ExecResult::ExecResultOK) {
            panic("failed to perform commit operation");
          }
          handler.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = tx})(
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

// Private diagnostic: sample exactly one preselected transaction per worker in
// each selected repetition. The callback and stock completion wait are left
// untouched. Each worker owns its cache line; no timestamps on other txs.
struct alignas(64) PaymentWorkerPhase {
  uint64_t txs = 0;
  uint64_t sample_index = 0;
  uint64_t tx_start_ns = 0;
  uint64_t handler_ready_ns = 0;
  uint64_t root_done_ns = 0;
  uint64_t commit_done_ns = 0;
  uint64_t emit_launch_done_ns = 0;
  uint64_t tx_done_ns = 0;
};
static_assert(sizeof(PaymentWorkerPhase) == 64);

struct PaymentRepPhase {
  explicit PaymentRepPhase(size_t workers_count) : workers(workers_count) {}
  std::vector<PaymentWorkerPhase> workers;
  uint64_t accounts = 0;
  uint64_t txs = 0;
  uint64_t threads = 0;
  uint64_t rep = 0;
  uint64_t worker_count = 0;
  uint64_t start_ns = 0;
  uint64_t end_ns = 0;
  uint64_t elapsed_us = 0;
};

uint64_t
payment_phase_now_ns()
{
  struct timespec time{};
  CHECK_EQ(clock_gettime(CLOCK_MONOTONIC, &time), 0);
  return static_cast<uint64_t>(time.tv_sec) * 1'000'000'000 +
         static_cast<uint64_t>(time.tv_nsec);
}

void
write_payment_phase_records(const char *path,
                            const std::vector<PaymentRepPhase> &records)
{
  std::ofstream out(path);
  CHECK(out.is_open());
  out << "PAYMENT_SPARSE_V1\n";
  for (const auto &r : records) {
    out << "rep\t" << r.accounts << '\t' << r.txs << '\t' << r.threads << '\t'
        << r.rep << '\t' << r.worker_count << '\t' << r.elapsed_us << '\t'
        << r.start_ns << '\t' << r.end_ns << '\n';
    for (size_t i = 0; i < r.worker_count; ++i) {
      const auto &w = r.workers[i];
      out << "sample\t" << r.accounts << '\t' << r.txs << '\t' << r.threads
          << '\t' << r.rep << '\t' << i << '\t' << w.txs << '\t'
          << w.sample_index << '\t' << w.tx_start_ns << '\t'
          << w.handler_ready_ns << '\t' << w.root_done_ns << '\t'
          << w.commit_done_ns << '\t' << w.emit_launch_done_ns << '\t'
          << w.tx_done_ns << '\n';
    }
  }
  out.flush();
  CHECK(out.good());
}

void
run_bench_rep_phase(BenchmarkState &bs,
                    std::span<SignedTransaction> total_work_txs,
                    int num_exec_threads, uint32_t num_txs_per_rep,
                    size_t num_reps, std::vector<PaymentRepPhase> &records)
{
  WorkerPool exec_pool(num_exec_threads);
  exec_pool.adjust_workers(num_exec_threads);
  if (total_work_txs.size() < num_txs_per_rep * num_reps) {
    panic("not enough transactions to run the benchmark");
  }
  assert((total_work_txs.size() % num_reps) == 0);
  assert(num_txs_per_rep == total_work_txs.size() / num_reps);

  // All storage for this configuration is allocated before its timed reps.
  const size_t record_base = records.size();
  for (size_t nrep = 0; nrep < num_reps; ++nrep) {
    records.emplace_back(num_exec_threads);
  }
  for (size_t nrep = 0U; nrep < num_reps; nrep++) {
    auto &phase = records[record_base + nrep];
    phase.accounts = bs.num_accounts_;
    phase.txs = num_txs_per_rep;
    phase.threads = num_exec_threads;
    phase.rep = nrep;
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

      auto &worker = phase.workers[static_cast<size_t>(i)];
      worker.txs = cur_count;
      // Spread samples over each worker's range, independently of tx contents
      // and observed timing. Rotate the positions between repetitions.
      worker.sample_index =
          (static_cast<uint64_t>(i) * 37 + nrep * 13) % cur_count;
      futures.emplace_back(exec_pool.submit([&bs, work_txs, offset, cur_count,
                                             base_height, &pending_events,
                                             &events_done, &worker]() -> void {
        auto runtime_context = make_hog_exec_context();
        auto sample_index = worker.sample_index;
        for (uint32_t j = 0; j < cur_count; j++) {
          const bool sample = j == sample_index;
          if (sample) {
            worker.tx_start_ns = payment_phase_now_ns();
          }
          {
            uint32_t k = offset + j;
            work_txs[k].tx.uniquifier =
                CommonBenchState::bench_uniquifier(base_height, k);
            auto const &tx = work_txs[k];

            auto handler =
                bs.make_handler(tx, bs.allocate_txid(), *runtime_context);
            if (sample) {
              worker.handler_ready_ns = payment_phase_now_ns();
            }
            if (!handler.root_invoke()().is_root_success()) {
              error("Failed to invoke transaction");
              panic("Failed to invoke transaction");
            }
            if (sample) {
              worker.root_done_ns = payment_phase_now_ns();
            }
            if (handler.try_commit_transaction()() !=
                ExecResult::ExecResultOK) {
              panic("failed to perform commit operation");
            }
            if (sample) {
              worker.commit_done_ns = payment_phase_now_ns();
            }
            handler.emit_events(ExecResult::ExecResultOK,
                                ProposedTransaction{.signed_tx = tx})(
                [&pending_events,
                 &events_done](this auto,
                               SystemStatus<void> res) -> x23::Detached {
                  if (!res.has_value()) {
                    panic("failed to emit events");
                  }
                  auto r =
                      pending_events.fetch_sub(1, std::memory_order_relaxed);
                  if (r == 1) {
                    events_done.store(true, std::memory_order_relaxed);
                    events_done.notify_one();
                  }
                  co_return;
                });
            if (sample) {
              worker.emit_launch_done_ns = payment_phase_now_ns();
            }
          } // Handler destruction remains at the end of this iteration.
          if (sample) {
            worker.tx_done_ns = payment_phase_now_ns();
          }
        }
      }));
    }
    phase.worker_count = futures.size();
    for (auto &f : futures) {
      f.wait();
    }
    events_done.wait(false);
    auto finish_time = std::chrono::steady_clock::now();
    // On Linux steady_clock is CLOCK_MONOTONIC, the same domain used by the
    // samples. Retain the stock time points with no extra clock reads.
    phase.start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         start_time.time_since_epoch())
                         .count();
    phase.end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       finish_time.time_since_epoch())
                       .count();
    phase.elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           finish_time - start_time)
                           .count();
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

// Coroutine-driven variant of run_bench_rep.
//
// run_bench_rep sizes a WorkerPool and blocks one OS thread per in-flight
// transaction, which is how the executor was driven when its database calls
// blocked. It no longer is: execution is coroutinized and runs on the shared
// x23 ThreadPool, which has one thread per core. Driving it with thousands of
// blocking OS threads measures the old shape -- oversubscription plus a wakeup
// of the blocked submitter on top of every reply -- rather than how the
// executor is now used.
//
// So this drives the same work the way a caller would today: `concurrency`
// coroutines on the pool, each awaiting its transactions rather than blocking
// on them. Concurrency is transactions in flight, decoupled from thread count.
void
run_bench_rep_coro(BenchmarkState &bs,
                   std::span<SignedTransaction> total_work_txs, int concurrency,
                   uint32_t num_txs_per_rep, size_t num_reps)
{
  if (total_work_txs.size() < num_txs_per_rep * num_reps) {
    panic("not enough transactions to run the benchmark");
  }

  for (size_t nrep = 0U; nrep < num_reps; nrep++) {
    info("bench rep {} for {} txs (accts {}) on {} coros", nrep,
         num_txs_per_rep, bs.num_accounts_, concurrency);
    auto work_txs =
        total_work_txs.subspan(nrep * num_txs_per_rep, num_txs_per_rep);

    const auto base_height = bs.block_height();
    auto start_time = std::chrono::steady_clock::now();

    std::atomic<uint32_t> next_tx = 0;
    std::atomic<int> live = concurrency;
    std::atomic<bool> all_done = false;
    std::atomic<uint64_t> pending_events = num_txs_per_rep;
    std::atomic<bool> events_done = false;

    for (int i = 0; i < concurrency; i++) {
      [](BenchmarkState &bs, std::span<SignedTransaction> work_txs,
         uint32_t num_txs_per_rep, BlockHeight base_height,
         std::atomic<uint32_t> &next_tx, std::atomic<int> &live,
         std::atomic<bool> &all_done, std::atomic<uint64_t> &pending_events,
         std::atomic<bool> &events_done) -> x23::Detached {
        co_await default_threadpool().bg();
        auto runtime_context = make_hog_exec_context();
        for (;;) {
          const uint32_t k = next_tx.fetch_add(1, std::memory_order_relaxed);
          if (k >= num_txs_per_rep) {
            break;
          }
          work_txs[k].tx.uniquifier =
              CommonBenchState::bench_uniquifier(base_height, k);
          auto const &tx = work_txs[k];

          auto handler =
              bs.make_handler(tx, bs.allocate_txid(), *runtime_context);
          if (!(co_await handler.root_invoke()).is_root_success()) {
            panic("Failed to invoke transaction");
          }
          if ((co_await handler.try_commit_transaction()) !=
              ExecResult::ExecResultOK) {
            panic("failed to perform commit operation");
          }
          handler.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = tx})(
              [&pending_events,
               &events_done](this auto,
                             SystemStatus<void> res) -> x23::Detached {
                if (!res.has_value()) {
                  panic("failed to emit events");
                }
                if (pending_events.fetch_sub(1, std::memory_order_relaxed) ==
                    1) {
                  events_done.store(true, std::memory_order_relaxed);
                  events_done.notify_one();
                }
                co_return;
              });
        }
        if (live.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          all_done.store(true, std::memory_order_relaxed);
          all_done.notify_one();
        }
      }(bs, work_txs, num_txs_per_rep, base_height, next_tx, live, all_done,
                                         pending_events, events_done);
    }

    all_done.wait(false);
    events_done.wait(false);
    auto finish_time = std::chrono::steady_clock::now();
    std::println("time taken: {} us for {} txs (accts {}) on {} coros",
                 std::chrono::duration_cast<std::chrono::microseconds>(
                     finish_time - start_time)
                     .count(),
                 num_txs_per_rep, bs.num_accounts_, concurrency);
    bs.clear_uniquifiers(num_txs_per_rep);
  }
}

void
run_payments_bench(std::vector<std::string> const &server_addrs,
                   uint32_t exec_port, uint32_t coord_port,
                   uint32_t num_accounts)
{
  // This is wayyy to slow if we generate and sign all the txs up front.
  // For now, we just reuse the transactions from one rep to the next.
  size_t num_reps = 10;
  uint32_t num_total_txs =
      std::max<uint32_t>(100000 * num_reps, 204800 * num_reps);
  BenchmarkState bs(server_addrs, exec_port, coord_port, num_accounts);
  auto work_txs = bs.initialize(num_total_txs);
  size_t offset = 0;
  const char *phase_file = std::getenv("HOGD_PAYMENT_PHASE_FILE");
  std::vector<PaymentRepPhase> phase_records;
  if (phase_file) {
    phase_records.reserve(20);
  }
  auto run_blocking = [&](std::span<SignedTransaction> txs, int threads,
                          uint32_t count) {
    if (phase_file && threads == 128) {
      run_bench_rep_phase(bs, txs, threads, count, num_reps, phase_records);
    }
    else {
      run_bench_rep(bs, txs, threads, count, num_reps);
    }
  };

  // strong scaling
  for (auto nthread : {64, 128, 256, 512, 1024, 2048}) {
    assert(offset + (100000 * num_reps) <= work_txs.size());
    run_blocking(std::span<SignedTransaction>(work_txs).subspan(offset,
                                                                100000 *
                                                                    num_reps),
                 nthread, 100000);
  }
  // weak scaling
  for (auto nthread : {64, 128, 256, 512, 1024, 2048}) {
    assert(offset + (100 * nthread * num_reps) <= work_txs.size());
    run_blocking(std::span<SignedTransaction>(work_txs).subspan(offset,
                                                                100 * nthread *
                                                                    num_reps),
                 nthread, 100 * nthread);
  }

  // Same strong-scaling work again, driven by coroutines instead of blocking
  // OS threads; see run_bench_rep_coro. Reported as "on N coros" so the two
  // shapes stay distinguishable in the metrics history.
  for (auto nconcurrent : {64, 128, 256, 512, 1024, 2048}) {
    assert(offset + (100000 * num_reps) <= work_txs.size());
    run_bench_rep_coro(bs,
                       std::span<SignedTransaction>(work_txs).subspan(
                           offset, 100000 * num_reps),
                       nconcurrent, 100000, num_reps);
  }
  // Emit after the entire sweep, not between timed repetitions.
  if (phase_file) {
    write_payment_phase_records(phase_file, phase_records);
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
               "<num_accounts>",
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

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "e:c:s:a:")) != -1) {
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
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (server_addrs.size() != 2 || !exec_port || !coord_port || !num_accounts) {
    usage(argv);
  }

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  hogd::run_payments_bench(server_addrs, *exec_port, *coord_port,
                           *num_accounts);
  // NOLINTEND(bugprone-unchecked-optional-access)

  std::fflush(stdout);
  return 0;
}
