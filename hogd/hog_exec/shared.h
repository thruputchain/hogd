#pragma once

#include <cstdint>

#include "hogd/metrics/shared.h"

namespace hogd {

inline Metrics::Counter successful_proposed_tx_count_metric;
inline Metrics::Counter failed_proposed_tx_count_metric;
inline Metrics::Counter gas_consumed_metric;
// Where a storage read's time goes: the RPC round trip to HogDb (which also
// covers the wait for the RPC reader to be scheduled), then the hop back onto
// the thread pool. Sums in nanoseconds; divide by db_read_count for means.
inline Metrics::Counter db_read_count_metric;
inline Metrics::Counter db_read_rpc_ns_metric;
inline Metrics::Counter db_read_hop_ns_metric;

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
  db_read_count_metric =
      Metrics::instance().per_thread_counter(Metrics::ComponentName::kExecutor,
                                             "db_read_count",
                                             "storage reads from HogDb");
  db_read_rpc_ns_metric = Metrics::instance().per_thread_counter(
      Metrics::ComponentName::kExecutor, "db_read_rpc_ns",
      "total ns from issuing a storage read to its reply being delivered");
  db_read_hop_ns_metric = Metrics::instance().per_thread_counter(
      Metrics::ComponentName::kExecutor, "db_read_hop_ns",
      "total ns a storage read waited to hop back onto the thread pool");
  Metrics::instance().mark_done_with_init(Metrics::ComponentName::kExecutor);
}

// Processes and tests that never call init_hog_exec_metrics() still read
// storage, so this is a no-op until the counters are registered.
inline void
record_db_read(uint64_t rpc_ns, uint64_t hop_ns)
{
  if (!db_read_count_metric) {
    return;
  }
  db_read_count_metric.increment();
  db_read_rpc_ns_metric.increment(rpc_ns);
  db_read_hop_ns_metric.increment(hop_ns);
}

inline bool
hog_exec_metrics_initialized()
{
  return Metrics::instance().done_with_init(Metrics::ComponentName::kExecutor);
}

} // namespace hogd
