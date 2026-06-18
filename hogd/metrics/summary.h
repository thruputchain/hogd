#pragma once

#include <atomic>
#include <format>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/metrics/histogram.h"
#include "hogd/metrics/metric.h"

namespace hogd {

template<typename T> class SummaryBuckets {
public:
  SummaryBuckets(T min, T max, size_t num_buckets,
                 std::vector<double> quantiles)
    : histogram_(min, max, num_buckets), quantiles_(std::move(quantiles))
  {
    CHECK(!quantiles_.empty());
    for (double quantile : quantiles_) {
      CHECK_GE(quantile, 0.0);
      CHECK_LE(quantile, 1.0);
    }
  }

  void add(const T &val)
  {
    histogram_.add(val);
    sum_.fetch_add(static_cast<double>(val), std::memory_order_relaxed);
  }

  size_t count() const { return histogram_.count(); }

  double sum() const { return sum_.load(std::memory_order_relaxed); }

  std::string quantile_upper_bound(double quantile) const
  {
    return histogram_.quantile_upper_bound(quantile);
  }

  std::string dump_to_string(const std::string &summary_name) const
  {
    std::ostringstream os;
    for (double quantile : quantiles_) {
      os << std::format("{}{{quantile=\"{}\"}} {}\n", summary_name, quantile,
                        quantile_upper_bound(quantile));
    }
    os << std::format("{}_sum {}\n", summary_name, sum());
    os << std::format("{}_count {}\n", summary_name, count());
    return os.str();
  }

private:
  HistogramBuckets<T> histogram_;
  std::vector<double> quantiles_;
  std::atomic<double> sum_ = 0;
};

struct SummaryImpl : Metric {
  SummaryImpl(std::string description, size_t min, size_t max,
              size_t num_buckets, std::vector<double> quantiles)
    : Metric(std::move(description)),
      summary(min, max, num_buckets, std::move(quantiles))
  {}

  void record(size_t value) { summary.add(value); }

  const char *prometheus_type() const override { return "summary"; }

  std::string dump_sample(const std::string &metric_name) override
  {
    return summary.dump_to_string(metric_name);
  }

  void dump_prometheus_value(std::ostringstream &os,
                             const std::string &metric_name) override
  {
    os << dump_sample(metric_name) << "\n";
  }

  SummaryBuckets<size_t> summary;
};

class Summary : public MetricHandle<SummaryImpl> {
public:
  Summary() = default;

  void record(size_t value) { metric().record(value); }

private:
  explicit Summary(SummaryImpl *metric) : MetricHandle(metric) {}

  friend class hogd::Metrics;
};

} // namespace hogd
