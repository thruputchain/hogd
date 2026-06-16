#pragma once

#include <array>
#include <atomic>
#include <format>
#include <memory>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "x23/corosync.h"

namespace hogd {

template<typename T> class Histogram {
public:
  Histogram(T min, T max, size_t num_buckets)
    : min_(min), max_(max),
      bucket_delta_((max_ - min_) / static_cast<double>(num_buckets)),
      buckets_(/*count=*/num_buckets, /*alloc=*/0)
  {}

  void add(const T &val)
  {
    if (val < min_) {
      underflow_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    else if (val > max_) {
      overflow_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    buckets_[bucket_idx(val)].fetch_add(1, std::memory_order_relaxed);
  }

private:
  size_t bucket_idx(const T &val) { return (val - min_) / bucket_delta_; }

  const T min_;
  const T max_;
  const double bucket_delta_;

  // Holds the number of elements in each bucket.
  std::vector<std::atomic<size_t>> buckets_;
  std::atomic<size_t> underflow_ = 0;
  std::atomic<size_t> overflow_ = 0;
};

} // namespace hogd
