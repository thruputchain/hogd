#include "hogd/metrics/new_metrics.h"

#include <format>
#include <sstream>
#include <string>

#include "hogd/base/types.h"
#include "x23/async.h"
#include "x23/corosync.h"

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

std::string
Metrics::dump_to_prometheus_string()
{
  std::ostringstream os;
  for (size_t i = 0; i < components_.size(); i++) {
    Component &component = components_[i];

    for (auto &[name, metric] : component.metrics) {
      auto it = component.descriptions.find(name);
      if (it != component.descriptions.end() && !it->second.empty()) {
        os << std::format("# HELP {} {}\n",
                          fully_qualified_name(static_cast<ComponentName>(i),
                                               name),
                          it->second);
      }
      // We only support gauges for now but can easily expand this to other
      // types.
      os << std::format("# TYPE {} gauge\n",
                        fully_qualified_name(static_cast<ComponentName>(i),
                                             name));
      os << std::format("{} {}\n",
                        fully_qualified_name(static_cast<ComponentName>(i),
                                             name),
                        metric.value());
    }
  }

  return os.str();
}

} // namespace hogd
