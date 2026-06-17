#include "hogd/metrics/metrics.h"

#include <format>
#include <sstream>
#include <string>

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

std::string
Metrics::dump_to_prometheus_string()
{
  std::ostringstream os;
  for (size_t i = 0; i < components_.size(); i++) {
    Component &component = components_[i];

    for (auto &[name, metric] : component.metrics) {
      const std::string full_name =
          fully_qualified_name(static_cast<ComponentName>(i), name);
      if (!metric->description.empty()) {
        os << std::format("# HELP {} {}\n", full_name, metric->description);
      }
      if (metric->type == MetricType::kHistogram) {
        os << std::format("# TYPE {} histogram\n", full_name);
        os << metric->dump_sample(full_name) << "\n";
      }
      else if (metric->type == MetricType::kCounter) {
        os << std::format("# TYPE {} counter\n", full_name);
        os << std::format("{} {}\n", full_name, metric->dump_sample(full_name));
      }
      else {
        os << std::format("# TYPE {} gauge\n", full_name);
        os << std::format("{} {}\n", full_name, metric->dump_sample(full_name));
      }
    }
  }

  return os.str();
}

} // namespace hogd
