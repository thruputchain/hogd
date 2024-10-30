#pragma once

// Uint128 reservation implementation.
// Can be swapped in for a regular uint128 implementation if and when we make
// one available as part of the base library.

#include <cstdint>

#include "hogd/base/compiler.h"

namespace hogd {

class UInt128Reservation {
public:
  UInt128Reservation &operator+=(uint64_t v)
  {
    if (integer_add_overflow<uint64_t>(lowbits_, v, &lowbits_)) {
      highbits_++;
    }
    
    return *this;
  }

  UInt128Reservation &operator-=(uint64_t v)
  {
    if (lowbits_ < v) {
      highbits_--;
    }
    lowbits_ -= v;
    return *this;
  }

  void clear()
  {
    lowbits_ = 0;
    highbits_ = 0;
  }

  uint64_t get_capped() const
  {
    if (highbits_ > 0) {
      return UINT64_MAX;
    }
    return lowbits_;
  }

private:
  uint64_t lowbits_ = 0, highbits_ = 0;
};

} // namespace hogd
