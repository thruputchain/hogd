#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "hogd/metrics/metrics.h"
#include "hogd/metrics/shared.h"

namespace hogd {

// Apply of an empty block is sub-millisecond. A large block can take seconds.
inline constexpr std::array<double, 16> kIndexerApplyDurationBounds = {
    0.0001, 0.0005, 0.001, 0.005, 0.01, 0.025, 0.05, 0.1,
    0.25,   0.5,    1,     2.5,   5,    10,    30,   60};

inline Metrics::Gauge indexer_contiguous_height;
inline Metrics::Histogram indexer_apply_duration_seconds;

inline void
init_indexer_metrics()
{
  Metrics &metrics = Metrics::instance();
  constexpr Metrics::ComponentName kComponent =
      Metrics::ComponentName::kIndexer;
  indexer_contiguous_height = metrics.gauge(
      kComponent, "contiguous_height",
      "Highest block height the indexer has applied contiguously.");
  indexer_apply_duration_seconds = metrics.histogram(
      kComponent, "apply_duration_seconds",
      "Seconds to durably apply one newly committed block. Duplicates are "
      "not recorded.",
      {kIndexerApplyDurationBounds.begin(), kIndexerApplyDurationBounds.end()});
  register_resident_bytes(kComponent);
  metrics.mark_done_with_init(kComponent);
}

inline void
record_indexer_height(int64_t contiguous_height)
{
  if (!indexer_contiguous_height) {
    return;
  }
  indexer_contiguous_height.record(static_cast<ssize_t>(contiguous_height));
}

inline void
record_indexer_apply(int64_t contiguous_height, double seconds)
{
  record_indexer_height(contiguous_height);
  if (indexer_apply_duration_seconds) {
    indexer_apply_duration_seconds.record(seconds);
  }
}

} // namespace hogd
