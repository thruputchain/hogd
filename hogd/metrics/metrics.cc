#include "hogd/metrics/metrics.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <utility>
#include <vector>

#include "hogd/base/types.h"

namespace hogd {

void
MetricRegistry::register_read_observer(std::function<void()> &&fn)
{
  std::scoped_lock guard(lock_);
  read_fns_.emplace_back(std::move(fn));
}

void
MetricRegistry::register_metric(std::pair<Metric, std::function<size_t()>> m)
{
  std::scoped_lock guard(lock_);
  auto &[metric, get_fn] = m;
  metrics_.emplace_back(std::move(metric));
  samplers_.emplace_back(metrics_.back().key, std::move(get_fn));
}

void
MetricRegistry::register_metrics(
    std::initializer_list<std::pair<Metric, std::function<size_t()>>> l)
{
  std::scoped_lock guard(lock_);
  size_t n = metrics_.size() + l.size();
  metrics_.reserve(n);
  samplers_.reserve(n);
  for (const auto &[metric, get_fn] : l) {
    metrics_.emplace_back(metric);
    samplers_.emplace_back(metrics_.back().key, get_fn);
  }
}

const std::vector<Metric> &
MetricRegistry::all()
{
  std::scoped_lock guard(lock_);
  return metrics_;
}

std::vector<Sample>
MetricRegistry::read()
{
  std::vector<Sample> samples;

  std::scoped_lock guard(lock_);

  // run read observer functions.
  std::ranges::for_each(read_fns_,
                        [](const std::function<void()> &fn) { fn(); });

  // gather samples
  samples.reserve(samplers_.size());
  for (const Sampler &s : samplers_) {
    samples.emplace_back(s.key, s.get_fn());
  }
  return samples;
}

} // namespace hogd
