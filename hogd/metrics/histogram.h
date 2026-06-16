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
    : min_(min), max_(max), buckets_(/*count=*/num_buckets, /*alloc=*/0)
  {}

private:
  const T min_;
  const T max_;

  // Holds the number of elements in each bucket.
  std::vector<size_t> buckets_;
};

} // namespace hogd
