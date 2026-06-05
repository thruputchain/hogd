#include "hogd/metrics/new_metrics.h"

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
  std::unique_ptr<std::unordered_map<std::string, Metric>> tmp;
  for (size_t i = 0; i < components_.size(); i++) {
    Component &component = components_[i];
    {
      x23::CoroGuard cg;
      co_await cg(component.lock);

      tmp = std::move(component.metrics);
      component.metrics =
          std::make_unique<std::unordered_map<std::string, Metric>>();
    }
    CHECK_PTR(tmp.get());
    CHECK_PTR(component.metrics.get());

    for (const auto &[name, metric] : *tmp) {
      for (size_t sample : metric.samples) {
        os << std::format("{}/{} {}\n", static_cast<ComponentName>(i), name,
                          sample);
      }
    }
  }

  co_return os.str();
}

} // namespace hogd
