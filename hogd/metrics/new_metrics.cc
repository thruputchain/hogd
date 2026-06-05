#include "hogd/metrics/new_metrics.h"

#include <format>
#include <sstream>
#include <string>
#include <unordered_map>

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

x23::Async<std::string>
Metrics::dump_to_prometheus_string()
{
  std::ostringstream os;
  std::unordered_map<std::string, Metric> tmp;
  for (size_t i = 0; i < components_.size(); i++) {
    Component &component = components_[i];
    {
      x23::CoroGuard cg;
      co_await cg(component.lock);

      tmp = component.metrics;
    }

    for (const auto &[name, metric] : tmp) {
      os << std::format("{}/{} {}\n", static_cast<ComponentName>(i), name,
                        metric.sample);
    }
  }

  co_return os.str();
}

} // namespace hogd
