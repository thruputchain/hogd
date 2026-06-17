#pragma once

#include <array>
#include <atomic>
#include <format>
#include <memory>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/metrics/histogram.h"
#include "x23/corosync.h"

namespace hogd {

class Metrics {
  struct Metric;
  struct GaugeMetric;
  struct CounterMetric;
  struct HistogramMetric;

public:
  enum class ComponentName {
    kExecutor,
    kDb,
    kMempool,
    // [kComponentCount] is used to determine the number of members in the enum.
    // It is not a component itself. Always keep this field last.
    kComponentCount
  };

private:
  enum class MetricType {
    kGauge,
    kCounter,
    kHistogram,
  };

  enum class CounterStorage {
    kAtomic,
    kPerThread,
  };

public:
  static constexpr size_t kNumComponents =
      static_cast<size_t>(ComponentName::kComponentCount);

  static Metrics &instance();

  // Disable copy and move.
  Metrics(const Metrics &) = delete;
  Metrics &operator=(const Metrics &) = delete;
  Metrics(Metrics &&) = delete;
  Metrics &operator=(Metrics &&) = delete;

  class Gauge {
  public:
    Gauge() = default;

    void record(ssize_t value)
    {
      CHECK(metric_ != nullptr);
      metric_->record(value);
    }
    ssize_t value()
    {
      CHECK(metric_ != nullptr);
      return metric_->value();
    }
    explicit operator bool() const { return metric_ != nullptr; }

  private:
    explicit Gauge(GaugeMetric *metric) : metric_(metric) {}

    GaugeMetric *metric_ = nullptr;

    friend class Metrics;
  };

  class Counter {
  public:
    Counter() = default;

    void increment(ssize_t delta = 1)
    {
      CHECK(metric_ != nullptr);
      metric_->increment(delta);
    }
    ssize_t value()
    {
      CHECK(metric_ != nullptr);
      return metric_->value();
    }
    explicit operator bool() const { return metric_ != nullptr; }

  private:
    explicit Counter(CounterMetric *metric) : metric_(metric) {}

    CounterMetric *metric_ = nullptr;

    friend class Metrics;
  };

  class HistogramHandle {
  public:
    HistogramHandle() = default;

    void record(size_t value)
    {
      CHECK(metric_ != nullptr);
      metric_->record(value);
    }
    explicit operator bool() const { return metric_ != nullptr; }

  private:
    explicit HistogramHandle(HistogramMetric *metric) : metric_(metric) {}

    HistogramMetric *metric_ = nullptr;

    friend class Metrics;
  };

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

  HistogramHandle histogram(ComponentName component_name,
                            const std::string &metric_name,
                            const std::string &description, size_t min,
                            size_t max, size_t num_buckets)
  {
    return HistogramHandle(register_histogram(component_name, metric_name,
                                              description, min, max,
                                              num_buckets));
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
  struct Metric {
    Metric(MetricType metric_type, std::string metric_description)
      : type(metric_type), description(std::move(metric_description))
    {}

    virtual ~Metric() = default;

    virtual std::string dump_sample(const std::string &metric_name) = 0;

    const MetricType type;
    const std::string description;
  };

  struct GaugeMetric : Metric {
    explicit GaugeMetric(std::string description)
      : Metric(MetricType::kGauge, std::move(description))
    {}

    void record(ssize_t metric_value)
    {
      value_.store(metric_value, std::memory_order_relaxed);
    }

    ssize_t value() const { return value_.load(std::memory_order_relaxed); }

    std::string dump_sample(const std::string &) override
    {
      return std::to_string(value());
    }

    std::atomic<ssize_t> value_ = 0;
  };

  struct CounterMetric : Metric {
    using Sample =
        std::variant<std::atomic<ssize_t>, PerThreadCounter<ssize_t>>;

    CounterMetric(std::string description, CounterStorage storage)
      : Metric(MetricType::kCounter, std::move(description))
    {
      switch (storage) {
      case CounterStorage::kAtomic:
        sample_.template emplace<std::atomic<ssize_t>>(0);
        return;
      case CounterStorage::kPerThread:
        sample_.template emplace<PerThreadCounter<ssize_t>>();
        return;
      }
      CHECK(false);
    }

    void increment(ssize_t delta)
    {
      std::visit(
          [delta](auto &sample) {
            using T = std::decay_t<decltype(sample)>;
            if constexpr (std::is_same_v<T, std::atomic<ssize_t>>) {
              sample.fetch_add(delta, std::memory_order_relaxed);
            }
            else if constexpr (std::is_same_v<T, PerThreadCounter<ssize_t>>) {
              sample.inc(delta);
            }
          },
          sample_);
    }

    ssize_t value()
    {
      return std::visit(
          [](auto &sample) -> ssize_t {
            using T = std::decay_t<decltype(sample)>;
            if constexpr (std::is_same_v<T, std::atomic<ssize_t>>) {
              return sample.load(std::memory_order_relaxed);
            }
            else if constexpr (std::is_same_v<T, PerThreadCounter<ssize_t>>) {
              return sample.get();
            }
          },
          sample_);
    }

    std::string dump_sample(const std::string &) override
    {
      return std::to_string(value());
    }

    Sample sample_;
  };

  struct HistogramMetric : Metric {
    HistogramMetric(std::string description, size_t min, size_t max,
                    size_t num_buckets)
      : Metric(MetricType::kHistogram, std::move(description)),
        histogram(min, max, num_buckets)
    {}

    void record(size_t value) { histogram.add(value); }

    std::string dump_sample(const std::string &metric_name) override
    {
      return histogram.dump_to_string(metric_name);
    }

    Histogram<size_t> histogram;
  };

  struct Component {
    std::unordered_map<std::string, std::unique_ptr<Metric>> metrics;
  };

  Metrics() = default;

  GaugeMetric *register_gauge(ComponentName component_name,
                              const std::string &metric_name,
                              const std::string &description)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(!done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    auto metric = std::make_unique<GaugeMetric>(description);
    GaugeMetric *metric_ptr = metric.get();
    auto [_, inserted] =
        component.metrics.try_emplace(metric_name, std::move(metric));
    CHECK(inserted);
    return metric_ptr;
  }

  CounterMetric *register_counter(ComponentName component_name,
                                  const std::string &metric_name,
                                  const std::string &description,
                                  CounterStorage storage)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(!done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    auto metric = std::make_unique<CounterMetric>(description, storage);
    CounterMetric *metric_ptr = metric.get();
    auto [_, inserted] =
        component.metrics.try_emplace(metric_name, std::move(metric));
    CHECK(inserted);
    return metric_ptr;
  }

  HistogramMetric *register_histogram(ComponentName component_name,
                                      const std::string &metric_name,
                                      const std::string &description,
                                      size_t min, size_t max,
                                      size_t num_buckets)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(!done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    auto metric =
        std::make_unique<HistogramMetric>(description, min, max, num_buckets);
    HistogramMetric *metric_ptr = metric.get();
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
