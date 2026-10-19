#pragma once

#include "hogd/metrics/shared.h"

namespace hogd {

inline Metrics::Counter successful_proposed_tx_count_metric;
inline Metrics::Counter failed_proposed_tx_count_metric;
inline Metrics::Counter gas_consumed_metric;

inline void
init_hog_exec_metrics()
{
  successful_proposed_tx_count_metric =
      Metrics::instance().per_thread_counter(Metrics::ComponentName::kExecutor,
                                             "successful_proposed_tx_count",
                                             "");
  failed_proposed_tx_count_metric =
      Metrics::instance().per_thread_counter(Metrics::ComponentName::kExecutor,
                                             "failed_proposed_tx_count", "");
  gas_consumed_metric =
      Metrics::instance().per_thread_counter(Metrics::ComponentName::kExecutor,
                                             "gas_consumed", "");
  Metrics::instance().mark_done_with_init(Metrics::ComponentName::kExecutor);
}

inline bool
hog_exec_metrics_initialized()
{
  return Metrics::instance().done_with_init(Metrics::ComponentName::kExecutor);
}

} // namespace hogd
