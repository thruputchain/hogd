#include "hogd/metrics/shared.h"

#include <cstddef>

#include "hogd/base/os.h"
#include "hogd/metrics/metrics.h"

namespace hogd {

namespace {

constexpr const char *kResidentBytesName = "resident_bytes";

} // namespace

void
register_resident_bytes(Metrics::ComponentName component_name)
{
  Metrics::instance().gauge(component_name, kResidentBytesName,
                            "Resident set size of this process, in bytes.");
}

void
sample_resident_bytes()
{
  const size_t bytes = resident_memory_bytes();
  if (bytes == 0) {
    return;
  }
  Metrics &metrics = Metrics::instance();
  for (size_t i = 0; i < Metrics::kNumComponents; i++) {
    const auto component_name = static_cast<Metrics::ComponentName>(i);
    // Gate access on initialization, as in dump_to_prometheus_string().
    if (!metrics.done_with_init(component_name)) {
      continue;
    }
    if (Metrics::Gauge gauge =
            metrics.find_gauge(component_name, kResidentBytesName)) {
      gauge.record(saturating_gauge_value(bytes));
    }
  }
}

} // namespace hogd
