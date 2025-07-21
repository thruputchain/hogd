// metrics.h - metric gathering framework
//
// TODO(abelay): Add histogram metric type.

#pragma once

#include <atomic>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hogd/base/types.h"

namespace hogd {

// Metric provides a description of a metric.
struct Metric {
  std::string key;
  std::string description;
  bool cumulative;
};

// Sample provides a recorded sample of a metric.
struct Sample {
  std::string_view key;
  size_t val;
};

// MetricSource is used to register a metric.
using MetricSource = std::pair<Metric, std::function<size_t()>>;

// MetricRegistry maintains a collection of metrics.
class MetricRegistry {
public:
  MetricRegistry() = default;
  MetricRegistry(std::span<MetricSource> sources,
                 std::span<std::function<void()>> read_fns = {})
    : sources_(sources.begin(), sources.end()),
      read_fns_(read_fns.begin(), read_fns.end())
  {}
  MetricRegistry(std::initializer_list<MetricSource> sources,
                 std::initializer_list<std::function<void()>> read_fns = {})
    : sources_(sources.begin(), sources.end()),
      read_fns_(read_fns.begin(), read_fns.end())
  {}

  template<std::unsigned_integral T, template<typename> typename Counter>
  void add(std::string key, std::string description, Counter<T> &cnt)
  {
    auto &&source =
        std::make_pair(Metric{.key = std::move(key),
                              .description = std::move(description),
                              .cumulative = true},
                       [&cnt] { return static_cast<size_t>(cnt.get()); });
    sources_.emplace_back(source);
  }

  template<std::unsigned_integral T>
  void add(std::string key, std::string description, bool cumulative,
           std::atomic<T> &val)
  {
    auto &&source = std::make_pair(Metric{.key = std::move(key),
                                          .description = std::move(description),
                                          .cumulative = cumulative},
                                   [&val] {
                                     return static_cast<size_t>(
                                         val.load(std::memory_order_relaxed));
                                   });
    sources_.emplace_back(source);
  }

  // get_metrics returns a description vector of supported metrics.
  std::vector<Metric> get_metrics() const;

  // read provides samples for all metric values.
  std::vector<Sample> read() const;

private:
  std::vector<MetricSource> sources_;
  std::vector<std::function<void()>> read_fns_;
};

// works with any class that has size_t get() and increments cumulatively.
template<std::unsigned_integral T, template<typename> typename Counter>
MetricSource
make_counter_metric(std::string key, std::string description, Counter<T> &cnt)
{
  return std::make_pair(Metric{.key = std::move(key),
                               .description = std::move(description),
                               .cumulative = true},
                        [&cnt] { return static_cast<size_t>(cnt.get()); });
}

// works with any integer reference.
template<std::unsigned_integral T>
MetricSource
make_raw_metric(std::string key, std::string description, bool cumulative,
                T &val)
{
  return std::make_pair(Metric{.key = std::move(key),
                               .description = std::move(description),
                               .cumulative = cumulative},
                        [&val] { return static_cast<size_t>(val); });
}

} // namespace hogd
