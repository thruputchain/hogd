// wide_math.h - exact multi-limb unsigned arithmetic for the math syscalls

#pragma once

#include <array>

#include "hogd/base/types.h"

// Unsigned integers wider than a machine word, as arrays of 64-bit limbs with
// the least significant limb first. Everything here is exact: nothing wraps,
// saturates, or rounds other than toward zero. The results are consensus, so
// validators must agree on every bit, and this file uses only integer
// operations whose results the language defines.
//
// The syscalls built on this are priced flat, so the work a routine does is
// set by the width of its operands. The values change only where a comparison
// stops.
namespace hogd {

// N limbs, least significant first.
//
// Compare values with limbs_less(): std::array's own operator< starts at limb
// 0, which is not numeric order.
template<size_t N> using Limbs = std::array<uint64_t, N>;

// a < b.
template<size_t N>
constexpr bool
limbs_less(const Limbs<N> &a, const Limbs<N> &b)
{
  for (size_t i = N; i-- > 0;) {
    if (a[i] != b[i])
      return a[i] < b[i];
  }
  return false;
}

// The full product a * b. Schoolbook: N * N limb products for any operands.
template<size_t N>
constexpr Limbs<2 * N>
limbs_mul(const Limbs<N> &a, const Limbs<N> &b)
{
  Limbs<2 * N> product{};
  for (size_t i = 0; i < N; ++i) {
    uint64_t carry = 0;
    for (size_t j = 0; j < N; ++j) {
      // Fits 128 bits: (2^64 - 1)^2 + 2 * (2^64 - 1) == 2^128 - 1.
      const uint128_t t = (uint128_t{a[i]} * b[j]) + product[i + j] + carry;
      product[i + j] = static_cast<uint64_t>(t);
      carry = static_cast<uint64_t>(t >> 64);
    }
    product[i + N] = carry;
  }
  return product;
}

// floor(sqrt(value)): the largest root with root * root <= value. It always
// fits half the limbs.
//
// Decides the root one bit at a time from the top, keeping a bit when the
// square still fits. 64 * N squarings for any value.
template<size_t N>
constexpr Limbs<N>
limbs_isqrt(const Limbs<2 * N> &value)
{
  Limbs<N> root{};
  for (size_t bit = 64 * N; bit-- > 0;) {
    Limbs<N> candidate = root;
    candidate[bit / 64] |= uint64_t{1} << (bit % 64);
    if (!limbs_less(value, limbs_mul(candidate, candidate)))
      root = candidate;
  }
  return root;
}

// The value of Bytes bytes, least significant first.
template<size_t Bytes> requires (Bytes % 8 == 0)
constexpr Limbs<Bytes / 8>
limbs_from_le_bytes(const std::array<uint8_t, Bytes> &bytes)
{
  Limbs<Bytes / 8> limbs{};
  for (size_t i = 0; i < Bytes; ++i)
    limbs[i / 8] |= uint64_t{bytes[i]} << (8 * (i % 8));
  return limbs;
}

constexpr uint128_t
limbs_to_u128(const Limbs<2> &limbs)
{
  return (uint128_t{limbs[1]} << 64) | limbs[0];
}

} // namespace hogd
