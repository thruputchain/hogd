#include "hogd/metrics/metrics.h"

#include <algorithm>
#include <functional>
#include <ranges>
#include <vector>

namespace hogd {

std::vector<Metric>
MetricRegistry::get_metrics() const
{
  auto tmp = sources_ | std::views::transform(&MetricSource::first);
  return {tmp.begin(), tmp.end()};
}

std::vector<Sample>
MetricRegistry::read() const
{
  // run read observer functions.
  std::ranges::for_each(read_fns_, [](const std::function<void()> &fn) {
    if (fn)
      fn();
  });

  // gather samples
  auto tmp = sources_ | std::views::transform([](const MetricSource &src) {
               return Sample{.key = src.first.key, .val = src.second()};
             });
  return {tmp.begin(), tmp.end()};
}

} // namespace hogd
