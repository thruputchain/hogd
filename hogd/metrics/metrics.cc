#include "hogd/metrics/metrics.h"

#include <format>
#include <limits>
#include <sstream>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"

namespace hogd {

Metrics &
Metrics::instance()
{
  static Metrics metrics;
  return metrics;
}

// static
std::string
Metrics::fully_qualified_name(Metrics::ComponentName component_name,
                              const std::string &metric_name)
{
  return std::format("{}_{}", component_name, metric_name);
}

void
Metrics::register_resident_bytes(ComponentName component_name)
{
  const auto index = static_cast<size_t>(component_name);
  CHECK_LT(index, kNumComponents);
  resident_gauges_[index] =
      gauge(component_name, "resident_bytes",
            "Resident set size of this process, in bytes.");
}

void
Metrics::sample_resident_bytes()
{
  const size_t bytes = resident_memory_bytes();
  if (bytes == 0) {
    return;
  }
  const auto max = static_cast<size_t>(std::numeric_limits<ssize_t>::max());
  const ssize_t recorded = bytes > max ? std::numeric_limits<ssize_t>::max()
                                       : static_cast<ssize_t>(bytes);
  for (size_t i = 0; i < resident_gauges_.size(); i++) {
    // Gate access on initialization, as in dump_to_prometheus_string().
    if (!done_with_init(static_cast<ComponentName>(i))) {
      continue;
    }
    Gauge &gauge = resident_gauges_[i];
    if (gauge) {
      gauge.record(recorded);
    }
  }
}

std::string
Metrics::dump_to_prometheus_string()
{
  std::ostringstream os;
  for (size_t i = 0; i < components_.size(); i++) {
    // Skip components still initializing. Iterating a component's metrics map
    // concurrently with a registration call's try_emplace is a data race (UB).
    // The acquire load here pairs with the release store in
    // mark_done_with_init(): once it observes true, every metric registered for
    // this component is fully constructed and visible, and registering more is
    // disallowed, so the map is safe to iterate without a lock.
    //
    // An uninitialized component's metrics are simply absent from the scrape;
    // monitoring can alert on the expected series being absent (gated on the
    // process's up metric) to catch a component that never came up.
    if (!done_with_init(static_cast<ComponentName>(i))) {
      continue;
    }

    Component &component = components_[i];

    for (auto &[name, metric] : component.metrics) {
      const std::string full_name =
          fully_qualified_name(static_cast<ComponentName>(i), name);
      if (!metric->description.empty()) {
        os << std::format("# HELP {} {}\n", full_name, metric->description);
      }
      os << std::format("# TYPE {} {}\n", full_name, metric->prometheus_type());
      metric->dump_prometheus_value(os, full_name);
    }
  }

  return os.str();
}

} // namespace hogd
