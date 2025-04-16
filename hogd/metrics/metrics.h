// metrics.h - metric gathering framework

#pragma once

#include <concepts>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
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
  std::string key;
  size_t val;
};

// MetricRegistry maintains a collection of metrics.
class MetricRegistry {
public:
  MetricRegistry() = default;
  MetricRegistry(std::vector<Metric> &&metrics) : metrics_(std::move(metrics))
  {}

  // register_read_observer adds a callback that runs before read(). This is
  // sometimes useful for refreshing metrics with the latest values.
  void register_read_observer(std::function<void()> &&fn);

  // register_metric registers a metric. Synchronization is provided.
  void register_metric(std::pair<Metric, std::function<size_t()>> m);

  // register_metrics registers a batch of metrics. Synchronization is provided.
  void register_metrics(
      std::initializer_list<std::pair<Metric, std::function<size_t()>>> l);

  // all returns a description vector of registered metrics.
  const std::vector<Metric> &all();

  // read provides samples for all metric values.
  std::vector<Sample> read();

private:
  struct Sampler {
    std::string key;
    std::function<size_t()> get_fn;
  };

  std::mutex lock_;
  std::vector<std::function<void()>> read_fns_;
  std::vector<Metric> metrics_;
  std::vector<Sampler> samplers_;
};

// works with any class that has size_t get() and increments cumulatively.
template<std::unsigned_integral T, template<typename> typename Counter>
std::pair<Metric, std::function<size_t()>>
make_counter_metric(std::string key, std::string description, Counter<T> &cnt)
{
  return std::make_pair(Metric{.key = std::move(key),
                               .description = std::move(description),
                               .cumulative = true},
                        [&cnt] { return static_cast<size_t>(cnt.get()); });
}

// works with any integer reference.
template<std::unsigned_integral T>
std::pair<Metric, std::function<size_t()>>
make_raw_metric(std::string key, std::string description, bool cumulative,
                T &val)
{
  return std::make_pair(Metric{.key = std::move(key),
                               .description = std::move(description),
                               .cumulative = cumulative},
                        [&val] { return static_cast<size_t>(val); });
}

} // namespace hogd
