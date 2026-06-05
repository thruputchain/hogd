#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "x23/corosync.h"

namespace hogd {

class Metrics {
public:
  enum class ComponentName {
    kExecutor,
    kDb,
    kMempool,
    // [kComponentCount] is used to determine the number of members in the enum.
    // It is not a component itself. Always keep this field last.
    kComponentCount
  };

  static constexpr size_t kNumComponents =
      static_cast<size_t>(ComponentName::kComponentCount);

  static Metrics &instance();

  // Disable copy and move.
  Metrics(const Metrics &) = delete;
  Metrics &operator=(const Metrics &) = delete;
  Metrics(Metrics &&) = delete;
  Metrics &operator=(Metrics &&) = delete;

  x23::Async<void> record(ComponentName component_name,
                          const std::string &metric_name, size_t metric_value)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    Component &metrics = components_[static_cast<size_t>(component_name)];
    x23::CoroGuard component_cg;
    co_await component_cg(component.lock);
    Metric &metric = metrics[metric_name];

    x23::CoroGuard cg;
    co_await cg(metric.lock);
    metric.samples.push_back(metric_value);
  }

private:
  struct Metric {
    x23::CoroLock lock;
    std::vector<size_t> samples;
  };

  struct Component {
    x23::CoroLock lock;
    std::unordered_map<std::string, Metric> metrics;
  };

  Metrics() = default;

  std::array<Component, kNumComponents> components_;
};

} // namespace hogd
