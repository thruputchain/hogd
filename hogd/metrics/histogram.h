#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <system_error>
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

template<typename T> class HistogramBuckets {
public:
  HistogramBuckets(T min, T max, size_t num_buckets)
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

  size_t count() const
  {
    size_t total = underflow_count() + overflow_count();
    for (size_t i = 0; i < buckets_.size(); i++) {
      total += bucket_count(i);
    }
    return total;
  }

  std::string quantile_upper_bound(double quantile) const
  {
    CHECK_GE(quantile, 0.0);
    CHECK_LE(quantile, 1.0);

    const size_t total_count = count();
    if (total_count == 0) {
      return "NaN";
    }

    const size_t target_count =
        std::max<size_t>(1, static_cast<size_t>(std::ceil(
                                quantile * static_cast<double>(total_count))));

    size_t cumulative_count = underflow_count();
    if (cumulative_count >= target_count) {
      return std::format("{}", min_);
    }

    for (size_t i = 0; i < buckets_.size(); i++) {
      cumulative_count += bucket_count(i);
      if (cumulative_count >= target_count) {
        return std::format("{}", bucket_upper_bound(i));
      }
    }

    return "+Inf";
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

  double bucket_upper_bound(size_t idx) const
  {
    return min_ + ((idx + 1) * bucket_delta_);
  }

  const T min_;
  const T max_;
  const double bucket_delta_;

  // Holds the number of elements in each bucket.
  std::vector<std::atomic<size_t>> buckets_;
  std::atomic<size_t> underflow_ = 0;
  std::atomic<size_t> overflow_ = 0;
};

// A Prometheus histogram with explicit upper bounds. Each bound is inclusive
// (`le`), values above the last bound land only in `+Inf`, and the exposition
// carries `_bucket`, `_sum` and `_count` with `_count` equal to `+Inf`.
//
// Bounds must be finite and strictly increasing. Record in base units
// (seconds, bytes) so the series follow Prometheus naming.
struct HistogramImpl : Metric {
  HistogramImpl(std::string description, std::vector<double> upper_bounds)
    : Metric(std::move(description)), upper_bounds_(std::move(upper_bounds)),
      // One extra slot for observations above the last bound.
      buckets_(upper_bounds_.size() + 1)
  {
    CHECK(!upper_bounds_.empty());
    for (size_t i = 0; i < upper_bounds_.size(); i++) {
      CHECK(std::isfinite(upper_bounds_[i]));
      if (i > 0) {
        CHECK_LT(upper_bounds_[i - 1], upper_bounds_[i]);
      }
      // Formatted once here, so an unprintable bound fails at registration
      // rather than on the first scrape.
      bound_labels_.push_back(format_bound(upper_bounds_[i]));
    }
    for (std::atomic<uint64_t> &bucket : buckets_) {
      bucket.store(0, std::memory_order_relaxed);
    }
  }

  void record(double value)
  {
    // NaN would corrupt _sum forever and belongs to no bucket.
    if (std::isnan(value)) {
      return;
    }
    const size_t idx = static_cast<size_t>(
        std::lower_bound(upper_bounds_.begin(), upper_bounds_.end(), value) -
        upper_bounds_.begin());
    buckets_[idx].fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(value, std::memory_order_relaxed);
  }

  uint64_t count() const
  {
    uint64_t total = 0;
    for (const std::atomic<uint64_t> &bucket : buckets_) {
      total += bucket.load(std::memory_order_relaxed);
    }
    return total;
  }

  const char *prometheus_type() const override { return "histogram"; }

  std::string dump_sample(const std::string &metric_name) override
  {
    std::ostringstream os;
    uint64_t cumulative = 0;
    for (size_t i = 0; i < upper_bounds_.size(); i++) {
      cumulative += buckets_[i].load(std::memory_order_relaxed);
      os << std::format("{}_bucket{{le=\"{}\"}} {}\n", metric_name,
                        bound_labels_[i], cumulative);
    }
    cumulative += buckets_.back().load(std::memory_order_relaxed);
    os << std::format("{}_bucket{{le=\"+Inf\"}} {}\n", metric_name, cumulative);
    os << std::format("{}_sum {}\n", metric_name,
                      sum_.load(std::memory_order_relaxed));
    // Derived from the same bucket loads, so a concurrent record() can never
    // make _count disagree with the +Inf bucket.
    os << std::format("{}_count {}\n", metric_name, cumulative);
    return os.str();
  }

  void dump_prometheus_value(std::ostringstream &os,
                             const std::string &metric_name) override
  {
    os << dump_sample(metric_name);
  }

private:
  // Shortest fixed-point form ("0.0001", "2.5", "60"), as Prometheus client
  // libraries write `le`; std::format("{}") would give "1e-04".
  static std::string format_bound(double bound)
  {
    std::array<char, 64> buf;
    const std::to_chars_result result =
        std::to_chars(buf.data(), buf.data() + buf.size(), bound,
                      std::chars_format::fixed);
    CHECK(result.ec == std::errc());
    return std::string(buf.data(), result.ptr);
  }

  const std::vector<double> upper_bounds_;
  std::vector<std::string> bound_labels_;
  std::vector<std::atomic<uint64_t>> buckets_;
  std::atomic<double> sum_ = 0.0;
};

class Histogram : public MetricHandle<HistogramImpl> {
public:
  Histogram() = default;

  void record(double value) { metric().record(value); }
  uint64_t count() { return metric().count(); }

private:
  explicit Histogram(HistogramImpl *metric) : MetricHandle(metric) {}

  friend class hogd::Metrics;
};

} // namespace hogd
