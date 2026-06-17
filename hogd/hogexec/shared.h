#pragma once

#include "hogd/metrics/shared.h"

namespace hogd {

inline Metrics::Counter successful_proposed_tx_count_metric;
inline Metrics::Counter failed_proposed_tx_count_metric;
inline Metrics::Counter gas_consumed_metric;

inline void
init_hogexec_metrics()
{
  successful_proposed_tx_count_metric = Metrics::instance().per_thread_counter(
      Metrics::ComponentName::kExecutor, "successful_proposed_tx_count", "");
  failed_proposed_tx_count_metric = Metrics::instance().per_thread_counter(
      Metrics::ComponentName::kExecutor, "failed_proposed_tx_count", "");
  gas_consumed_metric = Metrics::instance().per_thread_counter(
      Metrics::ComponentName::kExecutor, "gas_consumed", "");
  Metrics::instance().mark_done_with_init(Metrics::ComponentName::kExecutor);
}

inline bool
hogexec_metrics_initialized()
{
  return static_cast<bool>(successful_proposed_tx_count_metric) &&
         static_cast<bool>(failed_proposed_tx_count_metric) &&
         static_cast<bool>(gas_consumed_metric);
}

} // namespace hogd
