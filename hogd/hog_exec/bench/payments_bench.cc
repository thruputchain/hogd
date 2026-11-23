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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/bench/common_bench_state.h"
#include "hogd/hog_exec/bench/payment_db_queue_trace.h"
#include "hogd/hog_exec/bench/payment_db_timing.h"
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
    : CommonBenchState(server_addrs, exec_port, coord_port,
                       server_addrs.size() == 2 && server_addrs[0] == "hog11" &&
                               server_addrs[1] == "hog8" &&
                               (num_accounts == 2 || num_accounts == 10) &&
                               payment_db_timing_enabled("hog10")
                           ? num_accounts
                           : 0),
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

    constexpr uint32_t kQueueWorkSeed = 56013;
    const bool matched = payment_db_queue_pair("hog10");
    auto exp = ERC20ExperimentState(ERC20PaymentsConfig{.num_addresses =
                                                            num_accounts_},
                                    matched ? std::optional{kQueueWorkSeed}
                                            : std::nullopt);
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
    if (matched) {
      SHA256Hasher hasher;
      for (const auto &tx : work_txs)
        hasher.update(x23::serialize(tx));
      const auto digest = hasher.finalize();
      std::print("payment_db_workload {} {} {} ", kQueueWorkSeed,
                 WorkerPool::get_default_pool().worker_count(),
                 work_txs.size());
      for (const auto byte : digest)
        std::print("{:02x}", byte);
      std::println("");
    }
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

constexpr uint32_t kPaymentPhaseSampleStride = 256;

struct PaymentPhaseSample {
  int64_t invoke_ns{};
  int64_t commit_ns{};
  int64_t frame_lock_ns{};
  int64_t db_commit_ns{};
  int64_t commit_remaining_ns{};
};

struct PaymentDbCallerSample {
  TransactionID tx;
  int64_t frame_lock_ns;
  int64_t db_commit_ns;
  DbRpcAwaitTiming frame_rpc;
  DbRpcAwaitTiming commit_rpc;
};

using PaymentClock = std::chrono::steady_clock;

int64_t
elapsed_ns(PaymentClock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             PaymentClock::now() - start)
      .count();
}

int64_t
phase_percentile(std::vector<PaymentPhaseSample> const &samples,
                 int64_t PaymentPhaseSample::*member, size_t percentile)
{
  std::vector<int64_t> values;
  values.reserve(samples.size());
  for (const auto &sample : samples) {
    values.push_back(sample.*member);
  }
  std::sort(values.begin(), values.end());
  return values[(values.size() * percentile + 99) / 100 - 1];
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

  const bool record_phases =
      concurrency == 512 && num_txs_per_rep == 100'000 &&
      (bs.num_accounts_ == 2 || bs.num_accounts_ == 10) &&
      payment_db_timing_enabled("hog10");
  const auto queue_report = record_phases && payment_db_queue::records
                                ? payment_db_queue::report
                                : nullptr;
  for (size_t nrep = 0U; nrep < num_reps; nrep++) {
    info("bench rep {} for {} txs (accts {}) on {} coros", nrep,
         num_txs_per_rep, bs.num_accounts_, concurrency);
    auto work_txs =
        total_work_txs.subspan(nrep * num_txs_per_rep, num_txs_per_rep);

    // Only the coroutine claiming a transaction index can write its sample.
    std::vector<PaymentPhaseSample> phase_samples(
        record_phases ? (num_txs_per_rep + kPaymentPhaseSampleStride - 1) /
                            kPaymentPhaseSampleStride
                      : 0);
    std::vector<PaymentDbCallerSample> db_samples(phase_samples.size());
    std::atomic<size_t> db_sample_count = 0;
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
         std::atomic<bool> &events_done,
         std::span<PaymentPhaseSample> phase_samples,
         std::span<PaymentDbCallerSample> db_samples,
         decltype(queue_report) queue_report,
         std::atomic<size_t> &db_sample_count) -> x23::Detached {
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

          const TransactionID txid = bs.allocate_txid();
          auto handler = bs.make_handler(tx, txid, *runtime_context);
          const bool join_db =
              !db_samples.empty() && txid % kPaymentDbSampleStride == 0;
          PaymentPhaseSample *sample = nullptr;
          PaymentClock::time_point phase_start;
          if (!phase_samples.empty() && k % kPaymentPhaseSampleStride == 0) {
            sample = &phase_samples[k / kPaymentPhaseSampleStride];
            phase_start = PaymentClock::now();
          }
          const bool invoke_ok =
              (co_await handler.root_invoke()).is_root_success();
          if (sample) {
            sample->invoke_ns = elapsed_ns(phase_start);
          }
          if (!invoke_ok) {
            panic("Failed to invoke transaction");
          }
          if (sample) {
            phase_start = PaymentClock::now();
          }
          DbRpcAwaitTiming frame_rpc{.queue_report = queue_report};
          DbRpcAwaitTiming commit_rpc{.queue_report = queue_report};
          SystemCallHandler::CommitPhaseTiming commit_timing{
              .frame_rpc = join_db ? &frame_rpc : nullptr,
              .commit_rpc = join_db ? &commit_rpc : nullptr,
          };
          const auto commit_result = co_await handler.try_commit_transaction(
              (sample || join_db) ? &commit_timing : nullptr);
          if (sample) {
            sample->commit_ns = elapsed_ns(phase_start);
          }
          if (commit_result != ExecResult::ExecResultOK) {
            panic("failed to perform commit operation");
          }
          if (sample) {
            sample->frame_lock_ns = commit_timing.frame_lock_ns;
            sample->db_commit_ns = commit_timing.db_commit_ns;
            // Subtract within each transaction before computing percentiles.
            sample->commit_remaining_ns = sample->commit_ns -
                                          sample->frame_lock_ns -
                                          sample->db_commit_ns;
            CHECK(sample->commit_remaining_ns >= 0);
          }
          if (join_db) {
            const size_t index =
                db_sample_count.fetch_add(1, std::memory_order_relaxed);
            CHECK(index < db_samples.size());
            db_samples[index] = {txid, commit_timing.frame_lock_ns,
                                 commit_timing.db_commit_ns, frame_rpc,
                                 commit_rpc};
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
          all_done.store(true, std::memory_order_release);
          all_done.notify_one();
        }
      }(bs, work_txs, num_txs_per_rep, base_height, next_tx, live, all_done,
                                               pending_events, events_done,
                                               phase_samples, db_samples,
                                               queue_report, db_sample_count);
    }

    all_done.wait(false, std::memory_order_acquire);
    // Time the remaining wait as seen by this observer after waking.
    // Callbacks that have already drained contribute zero.
    std::optional<PaymentClock::time_point> event_tail_start;
    if (record_phases && !events_done.load(std::memory_order_relaxed)) {
      event_tail_start = PaymentClock::now();
    }
    events_done.wait(false);
    auto finish_time = PaymentClock::now();
    std::println("time taken: {} us for {} txs (accts {}) on {} coros",
                 std::chrono::duration_cast<std::chrono::microseconds>(
                     finish_time - start_time)
                     .count(),
                 num_txs_per_rep, bs.num_accounts_, concurrency);
    if (record_phases) {
      const auto event_tail_ns =
          event_tail_start
              ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                    finish_time - *event_tail_start)
                    .count()
              : 0;
      // Existing IDs 1–3 are invocation, commit and post-worker event wait.
      // IDs 4–6 partition same-transaction commit caller time.
      std::println("payment_phase {} {} 1 {} {} {}", bs.num_accounts_, nrep,
                   phase_samples.size(),
                   phase_percentile(phase_samples,
                                    &PaymentPhaseSample::invoke_ns, 50),
                   phase_percentile(phase_samples,
                                    &PaymentPhaseSample::invoke_ns, 90));
      std::println("payment_phase {} {} 2 {} {} {}", bs.num_accounts_, nrep,
                   phase_samples.size(),
                   phase_percentile(phase_samples,
                                    &PaymentPhaseSample::commit_ns, 50),
                   phase_percentile(phase_samples,
                                    &PaymentPhaseSample::commit_ns, 90));
      std::println("payment_phase {} {} 3 1 {} {}", bs.num_accounts_, nrep,
                   event_tail_ns, event_tail_ns);
      int phase = 4;
      for (const auto member : {&PaymentPhaseSample::frame_lock_ns,
                                &PaymentPhaseSample::db_commit_ns,
                                &PaymentPhaseSample::commit_remaining_ns}) {
        std::println("payment_phase {} {} {} {} {} {}", bs.num_accounts_, nrep,
                     phase++, phase_samples.size(),
                     phase_percentile(phase_samples, member, 50),
                     phase_percentile(phase_samples, member, 90));
      }
    }
    for (const auto &sample :
         std::span(db_samples).first(db_sample_count.load())) {
      std::println("payment_db_caller {} {} {} {} {} {} {} {} {} {}",
                   bs.num_accounts_, nrep, base_height, sample.tx,
                   sample.frame_lock_ns, sample.db_commit_ns,
                   sample.frame_rpc.client_await_ns,
                   sample.frame_rpc.threadpool_hop_ns,
                   sample.commit_rpc.client_await_ns,
                   sample.commit_rpc.threadpool_hop_ns);
    }
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
  if (payment_db_queue_enabled("hog10") &&
      (num_accounts == 2 || num_accounts == 10) && server_addrs.size() == 2 &&
      server_addrs[0] == "hog11" && server_addrs[1] == "hog8")
    payment_db_queue::start(num_accounts, "client");
  BenchmarkState bs(server_addrs, exec_port, coord_port, num_accounts);
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
