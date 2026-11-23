#pragma once

#include "hogd/metrics/shared.h"

namespace hogd {

inline Metrics::Counter successful_proposed_tx_count_metric;
inline Metrics::Counter failed_proposed_tx_count_metric;
inline Metrics::Counter successful_validation_tx_count_metric;
inline Metrics::Counter failed_validation_tx_count_metric;
inline Metrics::Counter gas_consumed_metric;

inline void
init_hog_exec_metrics()
{
  Metrics &metrics = Metrics::instance();
  constexpr Metrics::ComponentName kComponent =
      Metrics::ComponentName::kExecutor;
  successful_proposed_tx_count_metric =
      metrics.per_thread_counter(kComponent, "successful_proposed_tx_count",
                                 "");
  failed_proposed_tx_count_metric =
      metrics.per_thread_counter(kComponent, "failed_proposed_tx_count", "");
  successful_validation_tx_count_metric = metrics.per_thread_counter(
      kComponent, "successful_validation_tx_count",
      "Transactions that passed validation-path execution. The proposed-tx "
      "counters move only for the proposer.");
  failed_validation_tx_count_metric = metrics.per_thread_counter(
      kComponent, "failed_validation_tx_count",
      "Transactions that failed validation-path execution.");
  gas_consumed_metric =
      metrics.per_thread_counter(kComponent, "gas_consumed", "");
  register_resident_bytes(kComponent);
  metrics.mark_done_with_init(kComponent);
}

inline bool
hog_exec_metrics_initialized()
{
  return Metrics::instance().done_with_init(Metrics::ComponentName::kExecutor);
}

} // namespace hogd
