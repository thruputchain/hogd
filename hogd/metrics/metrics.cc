#include "hogd/metrics/metrics.h"

#include <format>
#include <sstream>
#include <string>

#include "hogd/base/types.h"

namespace hogd {

namespace {
// Synthetic gauge reporting whether each component has finished initialization.
// Emitted for every component regardless of init state.
constexpr const char *kComponentInitializedMetric =
    "hogd_metrics_component_initialized";
} // namespace

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

std::string
Metrics::dump_to_prometheus_string()
{
  std::ostringstream os;

  // Report each component's initialization status. This reads only the
  // done_with_init_ atomic, never a component's metrics map, so it is always
  // safe -- including for components still (or never) initializing. It lets
  // monitoring distinguish "component reported no such metric" from "component
  // never finished init", which the per-component skip below would otherwise
  // hide.
  os << std::format("# TYPE {} gauge\n", kComponentInitializedMetric);
  for (size_t i = 0; i < components_.size(); i++) {
    os << std::format("{}{{component=\"{}\"}} {}\n",
                      kComponentInitializedMetric,
                      static_cast<ComponentName>(i),
                      done_with_init(static_cast<ComponentName>(i)) ? 1 : 0);
  }

  for (size_t i = 0; i < components_.size(); i++) {
    // Skip the metrics of components still initializing. Iterating a
    // component's metrics map concurrently with a registration call's
    // try_emplace is a data race (UB). The acquire load here pairs with the
    // release store in mark_done_with_init(): once it observes true, every
    // metric registered for this component is fully constructed and visible,
    // and registering more is disallowed, so the map is safe to iterate without
    // a lock.
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
