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
    CHECK(!done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    CHECK(component.descriptions.insert({metric_name, description}).second);
  }
  void mark_done_with_init(ComponentName component_name, bool mark = true)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);

    std::atomic<bool> &component =
        done_with_init_[static_cast<size_t>(component_name)];
    bool expected = !mark;
    CHECK(component.compare_exchange_strong(expected, /*desired=*/mark,
                                            std::memory_order_release));
  }

  void record(ComponentName component_name, std::string metric_name,
              size_t metric_value)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    auto it = component.metrics.find(metric_name);
    CHECK(it != component.metrics.end());
    it->second.sample = metric_value;
  }

  size_t get(ComponentName component_name, std::string metric_name)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    auto it = component.metrics.find(metric_name);
    CHECK(it != component.metrics.end());
    return it->second.sample;
  }

  std::string dump_to_prometheus_string();

  void clear()
  {
    for (size_t i = 0; i < components_.size(); i++) {
      ComponentName component_name = static_cast<ComponentName>(i);
      CHECK(done_with_init(component_name));
      mark_done_with_init(component_name, /*mark=*/false);

      Component &component = components_[i];
      component.metrics.clear();
      component.descriptions.clear();

      mark_done_with_init(component_name, /*mark=*/true);
    }
  }

private:
  struct Metric {
    size_t sample;
  };

  struct Component {
    std::unordered_map<std::string, Metric> metrics;
    std::unordered_map<std::string, std::string> descriptions;
  };

  Metrics() = default;

  static std::string fully_qualified_name(ComponentName component_name,
                                          const std::string &metric_name);

  bool done_with_init(ComponentName component_name)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);

    std::atomic<bool> &component =
        done_with_init_[static_cast<size_t>(component_name)];
    return component.load(std::memory_order_acquire);
  }

  std::array<std::atomic<bool>, kNumComponents> done_with_init_{};
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
