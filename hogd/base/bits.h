// bits.h - bit hacks

#pragma once

#include <bit>
#include <cassert>
#include <compare>
#include <concepts>
#include <type_traits>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"

namespace hogd {

inline constexpr size_t kBitsPerByte = 8;

// AlignUp returns a value that is aligned upward to the nearest power of two.
template<std::unsigned_integral T>
constexpr T
AlignUp(T val, size_t align) noexcept
{
  ASSERT_CONSTEXPR(std::has_single_bit(align));
  return ((val + static_cast<T>(align) - 1) & ~static_cast<T>(align - 1));
}

// AlignDown returns a value that is aligned downward to the nearest power of
// two.
template<std::unsigned_integral T>
constexpr T
AlignDown(T val, size_t align) noexcept
{
  ASSERT_CONSTEXPR(std::has_single_bit(align));
  return (val & ~static_cast<T>(align - 1));
}

// DivideUp performs division but rounds up to the next highest integer when
// there is a remainder.
template<std::integral T>
constexpr T
DivideUp(T dividend, T divisor) noexcept
{
  return (dividend + divisor - 1) / divisor;
}

// WrappingSequence stores a monotonically increasing unsigned integer that is
// still valid for comparisons after overflow and wrap around.
//
// WARNING: The modular delta between any two WrappingSequence objects being
// compared must be smaller than the maximum signed value of T.
//
// This is an especially useful abstraction for sequence numbers like in TCP.
//
// Example:
//  WrappingSequence<uint8_t> s(0xff);
//  assert(s + 1 > s);
template<std::unsigned_integral T> struct WrappingSequence {
  T val;

  WrappingSequence() noexcept : val(0) {}
  explicit WrappingSequence(T val) noexcept : val(val) {}

  constexpr std::make_signed_t<T> operator-(const WrappingSequence &rhs) const
  {
    return static_cast<std::make_signed_t<T>>(val - rhs.val);
  }
  constexpr WrappingSequence operator+(const T &rhs) const
  {
    return WrappingSequence(val + rhs);
  }
  constexpr WrappingSequence &operator+=(const T &rhs)
  {
    val += rhs;
    return *this;
  }

  constexpr std::strong_ordering operator<=>(const WrappingSequence &rhs) const
  {
    auto cmp = static_cast<std::make_signed_t<T>>(val - rhs.val);
    if (cmp < 0)
      return std::strong_ordering::less;
    if (cmp == 0)
      return std::strong_ordering::equivalent;
    return std::strong_ordering::greater;
  }
  constexpr bool operator==(const WrappingSequence &rhs) const
  {
    return val == rhs.val;
  }
};

} // namespace hogd
