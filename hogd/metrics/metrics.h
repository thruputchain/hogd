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
          else {
            sample->store(metric_value, std::memory_order_relaxed);
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
          else {
            sample->fetch_add(delta, std::memory_order_relaxed);
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
    // [PerThread<T>] requires [T] to have a move constructor as it places
    // elements of type [T] into an [std::vector<T>]. [T = std::atomic<ssize_t>]
    // does not have a move constructor, so we have to implement the
    // [PerThreadAtomicSample] class below and add one ourselves.
    struct PerThreadAtomicSample {
      std::atomic<ssize_t> value{0};

      PerThreadAtomicSample() = default;

      // Disable copy.
      PerThreadAtomicSample(const PerThreadAtomicSample &) = delete;
      PerThreadAtomicSample &operator=(const PerThreadAtomicSample &) = delete;

      // Implement move.
      PerThreadAtomicSample(PerThreadAtomicSample &&other) noexcept
        : value(other.value.load(std::memory_order_relaxed))
      {
        other.value.store(0, std::memory_order_relaxed);
      }

      PerThreadAtomicSample &operator=(PerThreadAtomicSample &&other) noexcept
      {
        if (this == &other) [[unlikely]]
          return *this;
        value.store(other.value.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
        other.value.store(0, std::memory_order_relaxed);
        return *this;
      }

      void store(ssize_t metric_value, std::memory_order order)
      {
        value.store(metric_value, order);
      }

      ssize_t fetch_add(ssize_t delta, std::memory_order order)
      {
        return value.fetch_add(delta, order);
      }

      ssize_t load(std::memory_order order) const { return value.load(order); }
    };

    using Sample =
        std::variant<std::atomic<ssize_t>, PerThread<PerThreadAtomicSample>>;

    void init_sample(SampleType sample_type)
    {
      switch (sample_type) {
      case SampleType::kSsizeT:
        sample.template emplace<std::atomic<ssize_t>>(0);
        return;
      case SampleType::kPerThreadSsizeT:
        sample = PerThread<PerThreadAtomicSample>{};
        return;
      }
      CHECK(false);
    }

    ssize_t value()
    {
      return std::visit(
          [](auto &sample) -> ssize_t {
            using T = std::decay_t<decltype(sample)>;
            if constexpr (std::is_same_v<T, std::atomic<ssize_t>>) {
              return sample.load(std::memory_order_relaxed);
            }
            else {
              ssize_t total = 0;
              std::unique_lock ul = sample.lock_threads();
              sample.for_each(
                  ul, [&total](const PerThreadAtomicSample &thread_sample) {
                    total += thread_sample.load(std::memory_order_relaxed);
                  });
              return total;
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
