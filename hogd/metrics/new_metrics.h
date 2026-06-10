#pragma once

#include <array>
#include <format>
#include <memory>
#include <sstream>
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

  void init_metric(ComponentName component_name, const std::string &metric_name,
                   const std::string &description)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);

    Component &component = components_[static_cast<size_t>(component_name)];
    CHECK(component.descriptions.insert({metric_name, description}).second);
  }

  void record(ComponentName component_name, std::string metric_name,
              size_t metric_value)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);

    Component &component = components_[static_cast<size_t>(component_name)];
    component.metrics[metric_name].sample = metric_value;
  }

  size_t get(ComponentName component_name, std::string metric_name)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);

    Component &component = components_[static_cast<size_t>(component_name)];
    auto it = component.metrics.find(metric_name);
    CHECK(it != component.metrics.end());
    return it->second.sample;
  }

  std::string dump_to_prometheus_string();

  void clear()
  {
    for (Component &component : components_) {
      component.metrics.clear();
      component.descriptions.clear();
    }
  }

private:
  struct Metric {
    // [sample] is protected by the component lock. This synchronization is very
    // coarse-grained and we should ultimately replace this with a synchronized
    // map that uses [x23::CoroLock]'s rather than [std::mutex]'s.
    size_t sample;
  };

  struct Component {
    std::unordered_map<std::string, Metric> metrics;
    std::unordered_map<std::string, std::string> descriptions;
  };

  Metrics() = default;

  static std::string fully_qualified_name(ComponentName component_name,
                                          const std::string &metric_name);

  std::array<Component, kNumComponents> components_;
};

} // namespace hogd

template<>
struct std::formatter<hogd::Metrics::ComponentName>
  : std::formatter<const char *> {
  template<typename FormatContext>
  auto format(hogd::Metrics::ComponentName component_name,
              FormatContext &ctx) const
  {
    switch (component_name) {
    case hogd::Metrics::ComponentName::kExecutor:
      return std::formatter<const char *>::format("executor", ctx);
    case hogd::Metrics::ComponentName::kDb:
      return std::formatter<const char *>::format("db", ctx);
    case hogd::Metrics::ComponentName::kMempool:
      return std::formatter<const char *>::format("mempool", ctx);
    case hogd::Metrics::ComponentName::kComponentCount:
      CHECK(false);
      return std::formatter<const char *>::format("invalid", ctx);
    }
    CHECK(false);
    return std::formatter<const char *>::format("invalid", ctx);
  }
};
