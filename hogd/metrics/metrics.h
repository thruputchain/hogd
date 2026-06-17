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
#include <variant>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/metrics/histogram.h"
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

  enum class SampleType {
    kSsizeT,
    kPerThreadSsizeT,
    kHistogram,
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
                   const std::string &description, SampleType sample_type)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(!done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    CHECK(component.descriptions.insert({metric_name, description}).second);
    auto [it, inserted] = component.metrics.try_emplace(metric_name);
    CHECK(inserted);
    it->second.init_sample(sample_type);
  }

  void init_histogram(ComponentName component_name,
                      const std::string &metric_name,
                      const std::string &description, size_t min, size_t max,
                      size_t num_buckets)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(!done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    CHECK(component.descriptions.insert({metric_name, description}).second);
    auto [it, inserted] = component.metrics.try_emplace(metric_name);
    CHECK(inserted);
    it->second.init_histogram(min, max, num_buckets);
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

  void record(ComponentName component_name, const std::string &metric_name,
              ssize_t metric_value)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    auto it = component.metrics.find(metric_name);
    CHECK(it != component.metrics.end());
    std::visit(
        [metric_value](auto &sample) {
          using T = std::decay_t<decltype(sample)>;
          if constexpr (std::is_same_v<T, std::atomic<ssize_t>>) {
            sample.store(metric_value, std::memory_order_relaxed);
          }
          else if constexpr (std::is_same_v<T, PerThreadCounter<ssize_t>>) {
            // You cannot use [record] on a per-thread value as this does not
            // make sense. You would only be setting your own thread's value and
            // not touching the values set by other threads, so which value is
            // authoritative?
            //
            // You should either be calling [add] instead or using a single
            // [ssize_t].
            CHECK(false);
          }
          else if constexpr (std::is_same_v<T, Histogram<size_t>>) {
            sample.add(metric_value);
          }
          else {
            CHECK(false);
          }
        },
        it->second.sample);
  }

  void add(ComponentName component_name, const std::string &metric_name,
           ssize_t delta)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    auto it = component.metrics.find(metric_name);
    CHECK(it != component.metrics.end());
    std::visit(
        [delta](auto &sample) {
          using T = std::decay_t<decltype(sample)>;
          if constexpr (std::is_same_v<T, std::atomic<ssize_t>>) {
            sample.fetch_add(delta, std::memory_order_relaxed);
          }
          else if constexpr (std::is_same_v<T, PerThreadCounter<ssize_t>>) {
            sample.inc(delta);
          }
          else {
            CHECK(false);
          }
        },
        it->second.sample);
  }

  ssize_t get(ComponentName component_name, const std::string &metric_name)
  {
    CHECK_LT(static_cast<size_t>(component_name), kNumComponents);
    CHECK(done_with_init(component_name));

    Component &component = components_[static_cast<size_t>(component_name)];
    auto it = component.metrics.find(metric_name);
    CHECK(it != component.metrics.end());
    return it->second.value();
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
      component.descriptions.clear();
    }
  }

private:
  struct Metric {
    using Sample = std::variant<std::atomic<ssize_t>, PerThreadCounter<ssize_t>,
                                Histogram<size_t>>;

    void init_sample(SampleType sample_type)
    {
      switch (sample_type) {
      case SampleType::kSsizeT:
        sample.template emplace<std::atomic<ssize_t>>(0);
        return;
      case SampleType::kPerThreadSsizeT:
        sample.template emplace<PerThreadCounter<ssize_t>>();
        return;
      case SampleType::kHistogram:
        // TODO(jhumphri): Take this out and give each sample type its own
        // [init] method.
        sample.template emplace<Histogram<size_t>>(0, 10, 10);
        return;
      }
      CHECK(false);
    }

    void init_histogram(size_t min, size_t max, size_t num_buckets)
    {
      sample.template emplace<Histogram<size_t>>(min, max, num_buckets);
    }

    bool is_histogram() const
    {
      return std::holds_alternative<Histogram<size_t>>(sample);
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
            else {
              CHECK(false);
              return -1;
            }
          },
          sample);
    }

    std::string dump_to_string(const std::string &metric_name)
    {
      return std::visit(
          [&metric_name](auto &sample) -> std::string {
            using T = std::decay_t<decltype(sample)>;
            if constexpr (std::is_same_v<T, std::atomic<ssize_t>>) {
              return std::to_string(sample.load(std::memory_order_relaxed));
            }
            else if constexpr (std::is_same_v<T, PerThreadCounter<ssize_t>>) {
              return std::to_string(sample.get());
            }
            else if constexpr (std::is_same_v<T, Histogram<size_t>>) {
              return sample.dump_to_string(metric_name);
            }
            else {
              CHECK(false);
              return "";
            }
          },
          sample);
    }

    Sample sample;
  };

  struct Component {
    std::unordered_map<std::string, Metric> metrics;
    std::unordered_map<std::string, std::string> descriptions;
  };

  Metrics() = default;

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
