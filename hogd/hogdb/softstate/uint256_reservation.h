#pragma once

#include <concepts>
#include <limits>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"

namespace hogd {

// Uint256 reservation implementation.
// The NNInt primitive allows overflows in the positive direction,
// and caps the value at INT128_MAX in the next snapshot.
//
// Implementation requires tracking the total amount added to an nnint
// in a block, which requires an int larger than 128 bits.
//
// Templated impl because that makes the logic slightly clearer,
// although the only instantiation right now is T=uint128_t.
template<std::unsigned_integral T> class CappedOverflowReservation {
public:
  CappedOverflowReservation &operator+=(T v)
  {
    if (integer_add_overflow<T>(lowbits_, v, &lowbits_)) {
      highbits_++;
    }
    return *this;
  }

  CappedOverflowReservation &operator-=(T v)
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

  auto get_capped() const
  {
    if (highbits_ > 0) {
      return std::numeric_limits<T>::max();
    }
    return lowbits_;
  }

private:
  T lowbits_ = 0, highbits_ = 0;
};

using UInt256Reservation = CappedOverflowReservation<uint128_t>;

} // namespace hogd
