#pragma once

#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/metrics/metrics.h"
#include "rpc_types.hh"

namespace hogd {

class HogexecMetrics {
public:
  static void launch_log_worker();
  static void stop_log_worker();

  static void mark_tx_failed(InvokeResult const &result,
                             BlockExecutionMode mode)
  {
    switch (mode) {
    case BlockExecutionMode::kBlockExecutionModeProposal:
      mark_failed_proposed_tx();
      break;
    case BlockExecutionMode::kBlockExecutionModeValidation:
      mark_failed_validation_tx();
      break;
    case BlockExecutionMode::kBlockExecutionModeViewing:
      error("mark_tx_failed: received "
            "BlockExecutionMode::kBlockExecutionModeViewing");
      break;
    }
    mark_gas_consumed(result.gas_consumed_);
  }

  static void mark_tx_success(InvokeResult const &result,
                              BlockExecutionMode mode)
  {
    switch (mode) {
    case BlockExecutionMode::kBlockExecutionModeProposal:
      mark_successful_proposed_tx();
      break;
    case BlockExecutionMode::kBlockExecutionModeValidation:
      mark_successful_validation_tx();
      break;
    case BlockExecutionMode::kBlockExecutionModeViewing:
      error("mark_tx_success: received "
            "BlockExecutionMode::kBlockExecutionModeViewing");
      break;
    }
    mark_gas_consumed(result.gas_consumed_);
  }

  static void mark_successful_proposed_tx()
  {
    Metrics::instance().add(Metrics::ComponentName::kExecutor,
                            "successful_proposed_tx_count", 1);
  }
  static void mark_failed_proposed_tx()
  {
    Metrics::instance().add(Metrics::ComponentName::kExecutor,
                            "failed_proposed_tx_count", 1);
  }

  static void mark_successful_validation_tx()
  {
    // TODO(geoff): unimpl
  }
  static void mark_failed_validation_tx()
  {
    // TODO(geoff): unimpl
  }

  static void mark_gas_consumed(uint64_t gas)
  {
    Metrics::instance().add(Metrics::ComponentName::kExecutor, "gas_consumed",
                            static_cast<ssize_t>(gas));
  }

  ~HogexecMetrics() { stop_background_worker(); }

private:
  static HogexecMetrics &get_instance();

  struct MetricsSnapshot {
    ssize_t successful_proposed_tx_count = 0;
    ssize_t failed_proposed_tx_count = 0;
    ssize_t gas_consumed = 0;
    uint64_t time_ns = 0;

    MetricsSnapshot operator-(const MetricsSnapshot &other) const
    {
      return {.successful_proposed_tx_count =
                  successful_proposed_tx_count -
                  other.successful_proposed_tx_count,
              .failed_proposed_tx_count =
                  failed_proposed_tx_count - other.failed_proposed_tx_count,
              .gas_consumed = gas_consumed - other.gas_consumed,
              .time_ns = time_ns - other.time_ns};
    }
  };

  static MetricsSnapshot get_stats();
  void background_worker(std::stop_token &stoken);
  void stop_background_worker();

  std::jthread background_worker_;
  std::condition_variable_any cv_;
  std::mutex cv_mutex_;
  std::mutex background_worker_mutex_;
};

} // namespace hogd
