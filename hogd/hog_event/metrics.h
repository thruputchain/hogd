#pragma once

#include <cstdint>

#include "hogd/metrics/byte_histogram.h"
#include "hogd/metrics/metrics.h"
#include "hogd/metrics/shared.h"

namespace hogd {

// Null until init_event_metrics(). publish_committed_stored records only when
// the handles are live, so outbox tests that never init metrics stay no-ops.
inline Metrics::Histogram event_record_bytes;
inline Metrics::Gauge event_committed_height;

inline void
init_event_metrics()
{
  Metrics &metrics = Metrics::instance();
  constexpr Metrics::ComponentName kComponent = Metrics::ComponentName::kEvent;
  event_record_bytes = metrics.histogram(
      kComponent, "record_bytes",
      "Bytes of the indexer event-pull RPC record for a newly published "
      "committed block: consensus value, event logs, and transaction bodies. "
      "Distinct from mempool transaction-only bytes.",
      power_of_two_byte_bounds);
  event_committed_height = metrics.gauge(
      kComponent, "committed_height",
      "Height of the newest committed block published to the event outbox "
      "since this process started.");
  register_resident_bytes(kComponent);
  metrics.mark_done_with_init(kComponent);
}

// Records one new publication. An idempotent retry must not call this.
inline void
record_new_event_block(int64_t height, size_t record_bytes)
{
  if (!event_record_bytes) {
    return;
  }
  event_record_bytes.record(static_cast<double>(record_bytes));
  event_committed_height.record(static_cast<ssize_t>(height));
}

} // namespace hogd
