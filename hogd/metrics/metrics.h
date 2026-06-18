#pragma once

#include <array>
#include <atomic>
#include <format>
#include <memory>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/metrics/metric.h"

namespace hogd {

class Metrics;
} // namespace hogd

#include "hogd/metrics/counter.h"
#include "hogd/metrics/gauge.h"
#include "hogd/metrics/histogram.h"
#include "x23/corosync.h"

namespace hogd {

// This is a process-wide metrics registry. It stores gauges, counters, and
// histograms grouped by component, and can dump all registered metrics in
// Prometheus text format.
//
// Metrics should be registered during component initialization. Keep the handle
// returned by the registration call, then call [mark_done_with_init()] for that
// component. After a component is marked done with initialization, registering
// more metrics for that component is not allowed.
//
// Example:
// Metrics &metrics = Metrics::instance();
//
// Metrics::Gauge queue_depth =
//     metrics.gauge(Metrics::ComponentName::kExecutor, "queue_depth",
//                   "Current executor queue depth.");
// Metrics::Counter tx_count =
//     metrics.counter(Metrics::ComponentName::kExecutor, "tx_count",
//                     "Transactions processed.");
// Metrics::Histogram latency =
//     metrics.histogram(Metrics::ComponentName::kExecutor, "latency_ms",
//                       "Execution latency in milliseconds.", 0, 100, 10);
// metrics.mark_done_with_init(Metrics::ComponentName::kExecutor);
//
// queue_depth.record(17);
// tx_count.increment();
// latency.record(42);
//
// std::string prometheus = metrics.dump_to_prometheus_string();
class Metrics {
  using Metric = hogd::Metric;
  using GaugeImpl = hogd::GaugeImpl;
  using CounterImpl = hogd::CounterImpl;
  using HistogramImpl = hogd::HistogramImpl;
  using CounterStorage = hogd::CounterStorage;

public:
  using Gauge = hogd::Gauge;
  using Counter = hogd::Counter;
  using Histogram = hogd::Histogram;

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

  Gauge gauge(ComponentName component_name, const std::string &metric_name,
              const std::string &description)
  {
    return Gauge(register_gauge(component_name, metric_name, description));
  }

  Counter counter(ComponentName component_name, const std::string &metric_name,
                  const std::string &description)
  {
    return Counter(register_counter(component_name, metric_name, description,
                                    CounterStorage::kAtomic));
  }

  Counter per_thread_counter(ComponentName component_name,
                             const std::string &metric_name,
                             const std::string &description)
  {
    return Counter(register_counter(component_name, metric_name, description,
                                    CounterStorage::kPerThread));
  }

  Histogram histogram(ComponentName component_name,
                      const std::string &metric_name,
                      const std::string &description, size_t min, size_t max,
                      size_t num_buckets)
  {
    return Histogram(register_histogram(component_name, metric_name,
                                        description, min, max, num_buckets));
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

  std::string dump_to_prometheus_string();

  bool done_with_init(ComponentName component_name)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);

    std::atomic<bool> &component =
        done_with_init_[static_cast<size_t>(component_name)];
    return component.load(std::memory_order_acquire);
  }

  void clear()
  {
    for (size_t i = 0; i < components_.size(); i++) {
      done_with_init_[i].store(false, std::memory_order_release);

      Component &component = components_[i];
      component.metrics.clear();
    }
  }

private:
  struct Component {
    std::unordered_map<std::string, std::unique_ptr<Metric>> metrics;
  };

  Metrics() = default;

  GaugeImpl *register_gauge(ComponentName component_name,
                            const std::string &metric_name,
                            const std::string &description)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(!done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    auto metric = std::make_unique<GaugeImpl>(description);
    GaugeImpl *metric_ptr = metric.get();
    auto [_, inserted] =
        component.metrics.try_emplace(metric_name, std::move(metric));
    CHECK(inserted);
    return metric_ptr;
  }

  CounterImpl *register_counter(ComponentName component_name,
                                const std::string &metric_name,
                                const std::string &description,
                                CounterStorage storage)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(!done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    auto metric = std::make_unique<CounterImpl>(description, storage);
    CounterImpl *metric_ptr = metric.get();
    auto [_, inserted] =
        component.metrics.try_emplace(metric_name, std::move(metric));
    CHECK(inserted);
    return metric_ptr;
  }

  HistogramImpl *register_histogram(ComponentName component_name,
                                    const std::string &metric_name,
                                    const std::string &description, size_t min,
                                    size_t max, size_t num_buckets)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(!done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    auto metric =
        std::make_unique<HistogramImpl>(description, min, max, num_buckets);
    HistogramImpl *metric_ptr = metric.get();
    auto [_, inserted] =
        component.metrics.try_emplace(metric_name, std::move(metric));
    CHECK(inserted);
    return metric_ptr;
  }

  static std::string fully_qualified_name(ComponentName component_name,
                                          const std::string &metric_name);

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
