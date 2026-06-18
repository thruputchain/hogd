#pragma once

#include <atomic>
#include <format>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <type_traits>
#include <utility>
#include <variant>

#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/metrics/metric.h"

namespace hogd {

enum class CounterStorage {
  kAtomic,
  kPerThread,
};

struct CounterImpl : Metric {
  using Sample = std::variant<std::atomic<ssize_t>, PerThreadCounter<ssize_t>>;

  CounterImpl(std::string description, CounterStorage storage)
    : Metric(std::move(description))
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

  void increment(size_t delta)
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

  const char *prometheus_type() const override { return "counter"; }

  std::string dump_sample(const std::string & /*unused*/) override
  {
    return std::to_string(value());
  }

  void dump_prometheus_value(std::ostringstream &os,
                             const std::string &metric_name) override
  {
    os << std::format("{} {}\n", metric_name, dump_sample(metric_name));
  }

  Sample sample_;
};

class Counter : public MetricHandle<CounterImpl> {
public:
  Counter() = default;

  void increment(size_t delta = 1) { metric().increment(delta); }
  ssize_t value() { return metric().value(); }

private:
  explicit Counter(CounterImpl *metric) : MetricHandle(metric) {}

  friend class hogd::Metrics;
};

} // namespace hogd
