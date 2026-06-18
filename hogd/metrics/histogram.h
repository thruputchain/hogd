#pragma once

#include <array>
#include <atomic>
#include <cmath>
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
#include "hogd/metrics/metric.h"
#include "x23/corosync.h"

namespace hogd {

template<typename T> class Histogram {
public:
  Histogram(T min, T max, size_t num_buckets)
    : min_(min), max_(max),
      bucket_delta_((max_ - min_) / static_cast<double>(num_buckets)),
      buckets_(/*count=*/num_buckets)
  {
    for (std::atomic<size_t> &bucket : buckets_) {
      bucket.store(0, std::memory_order_relaxed);
    }
  }

  void add(const T &val)
  {
    if (val <= min_) {
      underflow_.fetch_add(1, std::memory_order_relaxed);
    }
    else if (val > max_) {
      overflow_.fetch_add(1, std::memory_order_relaxed);
    }
    else {
      buckets_[bucket_idx(val)].fetch_add(1, std::memory_order_relaxed);
    }
  }

  size_t bucket_count(size_t idx) const
  {
    return buckets_[idx].load(std::memory_order_relaxed);
  }

  size_t underflow_count() const
  {
    return underflow_.load(std::memory_order_relaxed);
  }

  size_t overflow_count() const
  {
    return overflow_.load(std::memory_order_relaxed);
  }

  std::string dump_to_string(const std::string &histogram_name) const
  {
    std::ostringstream os;
    const std::string bucket_name = std::format("{}_bucket", histogram_name);

    size_t cumulative_count = underflow_count();
    os << std::format("{}{{le=\"{}\"}} {}\n", bucket_name, min_,
                      cumulative_count);
    for (size_t i = 0; i < buckets_.size(); i++) {
      const double boundary = min_ + ((i + 1) * bucket_delta_);
      cumulative_count += bucket_count(i);
      os << std::format("{}{{le=\"{}\"}} {}\n", bucket_name, boundary,
                        cumulative_count);
    }
    cumulative_count += overflow_count();
    os << std::format("{}{{le=\"+Inf\"}} {}\n", bucket_name, cumulative_count);
    os << std::format("{}_count {}\n", histogram_name, cumulative_count);
    return os.str();
  }

private:
  size_t bucket_idx(const T &val)
  {
    return static_cast<size_t>(std::ceil((val - min_) / bucket_delta_) - 1);
  }

  const T min_;
  const T max_;
  const double bucket_delta_;

  // Holds the number of elements in each bucket.
  std::vector<std::atomic<size_t>> buckets_;
  std::atomic<size_t> underflow_ = 0;
  std::atomic<size_t> overflow_ = 0;
};

namespace metrics_detail {

struct HistogramImpl : Metric {
  HistogramImpl(std::string description, size_t min, size_t max,
                size_t num_buckets)
    : Metric(std::move(description)), histogram(min, max, num_buckets)
  {}

  void record(size_t value) { histogram.add(value); }

  const char *prometheus_type() const override { return "histogram"; }

  std::string dump_sample(const std::string &metric_name) override
  {
    return histogram.dump_to_string(metric_name);
  }

  void dump_prometheus_value(std::ostringstream &os,
                             const std::string &metric_name) override
  {
    os << dump_sample(metric_name) << "\n";
  }

  Histogram<size_t> histogram;
};

class HistogramHandle : public MetricHandle<HistogramImpl> {
public:
  HistogramHandle() = default;

  void record(size_t value) { metric().record(value); }

private:
  explicit HistogramHandle(HistogramImpl *metric) : MetricHandle(metric) {}

  friend class hogd::Metrics;
};

} // namespace metrics_detail

} // namespace hogd
