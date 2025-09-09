#pragma once

#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/metrics/metrics.h"
#include "rpc_types.hh"

namespace hogd {

class HogexecMetrics {
public:
  static void launch_log_worker();

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
    }
    mark_gas_consumed(result.gas_consumed_);
  }

  static void mark_successful_proposed_tx()
  {
    get_instance().successful_proposed_tx_count_.inc();
  }
  static void mark_failed_proposed_tx()
  {
    get_instance().failed_proposed_tx_count_.inc();
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
    get_instance().gas_consumed_.inc(gas);
  }

private:
  static HogexecMetrics &get_instance();

  struct MetricsSnapshot {
    size_t successful_proposed_tx_count = 0;
    size_t failed_proposed_tx_count = 0;
    uint64_t gas_consumed = 0;
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

  MetricsSnapshot get_stats();
  void background_worker(std::stop_token &stoken);

  PerThreadCounter<size_t> successful_proposed_tx_count_;
  PerThreadCounter<size_t> failed_proposed_tx_count_;
  PerThreadCounter<uint64_t> gas_consumed_;

  MetricRegistry registry_ =
      MetricRegistry({make_counter_metric("successful_proposed_tx_count", "",
                                          successful_proposed_tx_count_),
                      make_counter_metric("failed_proposed_tx_count", "",
                                          failed_proposed_tx_count_),
                      make_counter_metric("gas_consumed", "", gas_consumed_)});
  std::jthread background_worker_;
  std::condition_variable_any cv_;
  std::mutex cv_mutex_;
};

} // namespace hogd
