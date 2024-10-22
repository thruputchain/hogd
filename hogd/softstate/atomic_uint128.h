#pragma once

// Atomic uint128 implementation.  Avoids dword locks
// but does not guarantee correctness of reads
// when being actively written.

#include <atomic>
#include <cstdint>

#include "hogd/base/compiler.h"

namespace hogd {

class AtomicUint128 {
public:
  void add(uint64_t v)
  {
    uint64_t prev = lowbits_.fetch_add(v, std::memory_order_relaxed);

    if (integer_add_overflow_p(prev, v)) {
      highbits_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void sub(uint64_t v)
  {
    uint64_t prev = lowbits_.fetch_sub(v, std::memory_order_relaxed);
    if (prev < v) {
      highbits_.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  void clear()
  {
    lowbits_ = 0;
    highbits_ = 0;
  }

  uint64_t get_capped() const
  {
    if (highbits_.load(std::memory_order_relaxed) > 0) {
      return UINT64_MAX;
    }
    return lowbits_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<uint64_t> lowbits_ = 0, highbits_ = 0;
};

} // namespace hogd
