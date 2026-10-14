#pragma once

#include <iosfwd>
#include <string>
#include <utility>

#include "hogd/base/log.h"

namespace hogd {

class Metrics;

struct Metric {
  explicit Metric(std::string metric_description)
    : description(std::move(metric_description))
  {}

  virtual ~Metric() = default;

  virtual const char *prometheus_type() const = 0;
  virtual std::string dump_sample(const std::string &metric_name) = 0;
  virtual void dump_prometheus_value(std::ostringstream &os,
                                     const std::string &metric_name) = 0;

  const std::string description;
};

template<typename MetricImpl> class MetricHandle {
public:
  MetricHandle() = default;

  explicit operator bool() const { return metric_ != nullptr; }

protected:
  explicit MetricHandle(MetricImpl *metric) : metric_(metric) {}

  MetricImpl &metric()
  {
    CHECK_PTR(metric_);
    return *metric_;
  }

private:
  MetricImpl *metric_ = nullptr;
};

} // namespace hogd
