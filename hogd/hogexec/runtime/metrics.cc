#include "hogd/hogexec/runtime/metrics.h"

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include "hogd/base/log.h"
#include "hogd/hogexec/shared.h"
#include "hogd/metrics/metrics.h"
#include "x23/fastclock.h"

namespace hogd {

void
HogexecMetrics::launch_log_worker()
{
  if (!hogexec_metrics_initialized()) {
    throw std::runtime_error(
        "HogexecMetrics::launch_log_worker: executor metrics not initialized; "
        "call init_hogexec_metrics() (hogd/hogexec/shared.h) first");
  }

  HogexecMetrics &instance = get_instance();
  std::scoped_lock g(instance.background_worker_mutex_);
  if (!instance.background_worker_.joinable()) {
    instance.background_worker_ = std::jthread([](std::stop_token stoken) {
      get_instance().background_worker(stoken);
    });
  }
}

void
HogexecMetrics::stop_log_worker()
{
  get_instance().stop_background_worker();
}

HogexecMetrics &
HogexecMetrics::get_instance()
{
  static HogexecMetrics instance;
  return instance;
}

// static
HogexecMetrics::MetricsSnapshot
HogexecMetrics::get_stats()
{
  MetricsSnapshot snapshot{};
  snapshot.time_ns = x23::fastclock::get_ns();

  snapshot.successful_proposed_tx_count =
      successful_proposed_tx_count_metric.value();
  snapshot.failed_proposed_tx_count = failed_proposed_tx_count_metric.value();
  snapshot.gas_consumed = gas_consumed_metric.value();

  return snapshot;
}

void
HogexecMetrics::background_worker(std::stop_token &stoken)
{
  MetricsSnapshot snapshot{
      .time_ns = x23::fastclock::get_ns(),
  };
  while (!stoken.stop_requested()) {
    std::unique_lock g(cv_mutex_);
    cv_.wait_for(g, stoken, std::chrono::seconds(1), [] { return false; });
    if (stoken.stop_requested()) {
      break;
    }

    auto new_snapshot = get_stats();
    auto delta = new_snapshot - snapshot;
    double time_delta_s = delta.time_ns / 1'000'000'000.0;

    info("delta: {} ({:.2f}/s) successful proposed txs, {} ({:.2f}/s) failed "
         "proposed txs, {} ({:.2f}/s) gas consumed, {} duration",
         delta.successful_proposed_tx_count,
         (delta.successful_proposed_tx_count) / time_delta_s,
         delta.failed_proposed_tx_count,
         delta.failed_proposed_tx_count / time_delta_s, delta.gas_consumed,
         delta.gas_consumed / time_delta_s, time_delta_s);
    snapshot = new_snapshot;
  }
}

void
HogexecMetrics::stop_background_worker()
{
  std::scoped_lock g(background_worker_mutex_);
  if (!background_worker_.joinable()) {
    return;
  }
  background_worker_.request_stop();
  cv_.notify_all();
  background_worker_.join();
}

} // namespace hogd
