#include "hogd/hogexec/runtime/metrics.h"

#include <chrono>
#include <mutex>
#include <stop_token>
#include <thread>

#include "hogd/base/log.h"
#include "x23/fastclock.h"

namespace hogd {

void
HogexecMetrics::launch_log_worker()
{
  auto &instance = get_instance();
  if (!instance.background_worker_.joinable()) {
    instance.background_worker_ = std::jthread([](std::stop_token stoken) {
      get_instance().background_worker(stoken);
    });
  }
}

HogexecMetrics &
HogexecMetrics::get_instance()
{
  static HogexecMetrics instance;
  return instance;
}

HogexecMetrics::MetricsSnapshot
HogexecMetrics::get_stats()
{
  auto samples = registry_.read();
  MetricsSnapshot snapshot{};
  snapshot.time_ns = x23::fastclock::get_ns();
  for (auto &sample : samples) {
    if (sample.key == "successful_proposed_tx_count") {
      snapshot.successful_proposed_tx_count = sample.val;
    }
    else if (sample.key == "failed_proposed_tx_count") {
      snapshot.failed_proposed_tx_count = sample.val;
    }
    else if (sample.key == "gas_consumed") {
      snapshot.gas_consumed = sample.val;
    }
  }
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

} // namespace hogd
