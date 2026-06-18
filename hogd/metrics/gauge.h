#pragma once

#include <atomic>
#include <format>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <utility>

namespace hogd::metrics_detail {

struct GaugeImpl : Metric {
  explicit GaugeImpl(std::string description) : Metric(std::move(description))
  {}

  void record(ssize_t metric_value)
  {
    value_.store(metric_value, std::memory_order_relaxed);
  }

  ssize_t value() const { return value_.load(std::memory_order_relaxed); }

  const char *prometheus_type() const override { return "gauge"; }

  std::string dump_sample(const std::string & /*unused*/) override
  {
    return std::to_string(value());
  }

  void dump_prometheus_value(std::ostringstream &os,
                             const std::string &metric_name) override
  {
    os << std::format("{} {}\n", metric_name, dump_sample(metric_name));
  }

  std::atomic<ssize_t> value_ = 0;
};

class Gauge : public MetricHandle<GaugeImpl> {
public:
  Gauge() = default;

  void record(ssize_t value) { metric().record(value); }
  ssize_t value() { return metric().value(); }

private:
  explicit Gauge(GaugeImpl *metric) : MetricHandle(metric) {}

  friend class hogd::Metrics;
};

} // namespace hogd::metrics_detail
