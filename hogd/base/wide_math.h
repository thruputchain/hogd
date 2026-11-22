// wide_math.h - exact multi-limb unsigned arithmetic for the math syscalls

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <optional>

#include "hogd/base/bits.h"
#include "hogd/base/types.h"

// Unsigned integers wider than a machine word, as 64-bit limbs with the least
// significant first. The results are consensus, so every operation is exact
// and uses only integer arithmetic the language defines. The syscalls built on
// this are priced flat: the work is bounded by the operands' width.
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

// value * 2^shift for a shift below 64 * N. Bits shifted past the top are
// dropped.
template<size_t N>
constexpr Limbs<N>
limbs_shl(const Limbs<N> &value, size_t shift)
{
  const size_t limbs = shift / 64;
  const size_t bits = shift % 64;
  Limbs<N> out{};
  for (size_t i = N; i-- > limbs;) {
    out[i] = value[i - limbs] << bits;
    if (bits != 0 && i > limbs)
      out[i] |= value[i - limbs - 1] >> (64 - bits);
  }
  return out;
}

// floor((high * 2^64 + low) / divisor) for high < divisor, which is what makes
// the quotient fit a limb.
constexpr uint64_t
limb_div(uint64_t high, uint64_t low, uint64_t divisor)
{
  return static_cast<uint64_t>(((uint128_t{high} << 64) | low) / divisor);
}

template<size_t N> struct LimbsDivRem {
  Limbs<2 * N> quotient;
  Limbs<N> remainder;

  constexpr bool operator==(const LimbsDivRem &) const = default;
};

// floor(dividend / divisor) and dividend mod divisor, or nullopt for a zero
// divisor.
//
// Knuth, The Art of Computer Programming, volume 2, section 4.3.1, Algorithm
// D, with b = 2^64. The step names are Knuth's, and so are u, v, n, j, qhat
// and rhat.
template<size_t N>
constexpr std::optional<LimbsDivRem<N>>
limbs_div_rem(const Limbs<2 * N> &dividend, const Limbs<N> &divisor)
{
  // The limbs the divisor has. Algorithm D wants a nonzero top limb.
  size_t n = N;
  while (n > 0 && divisor[n - 1] == 0)
    --n;
  if (n == 0)
    return std::nullopt;

  LimbsDivRem<N> result{};
  if (n == 1) {
    // Algorithm D needs two divisor limbs. One is short division: the
    // remainder goes down the dividend a limb at a time. It stays below the
    // divisor, so each digit fits a limb.
    uint64_t rem = 0;
    for (size_t j = 2 * N; j-- > 0;) {
      const uint64_t digit = limb_div(rem, dividend[j], divisor[0]);
      result.quotient[j] = digit;
      // Below 2^64, so the low limb of the subtraction is all of it.
      rem = dividend[j] - (digit * divisor[0]);
    }
    result.remainder[0] = rem;
    return result;
  }

  // D1. Normalize: shift both left until the divisor's top bit is set, which
  // is what bounds the error of the estimate in D3. The quotient is unchanged,
  // and the dividend can grow by a limb.
  const int shift = std::countl_zero(divisor[n - 1]);
  // What a limb passes to the one above: value >> (64 - shift), written so
  // that no shift at all, where that count is 64, passes nothing.
  const auto passed_up = [shift](uint64_t value) {
    return (value >> 1) >> (63 - shift);
  };
  Limbs<N> v{};
  v[0] = divisor[0] << shift;
  for (size_t i = 1; i < n; ++i)
    v[i] = (divisor[i] << shift) | passed_up(divisor[i - 1]);
  Limbs<(2 * N) + 1> u{};
  u[0] = dividend[0] << shift;
  for (size_t i = 1; i < 2 * N; ++i)
    u[i] = (dividend[i] << shift) | passed_up(dividend[i - 1]);
  u[2 * N] = passed_up(dividend[(2 * N) - 1]);

  // D2, D7. One quotient digit a pass, most significant first. A pass divides
  // the n + 1 limbs u[j .. j + n] by v and leaves the remainder in them. That
  // remainder is below v, so the next pass starts with u[j + n] <= v[n - 1].
  // So does the first: u[2 * N] holds only the bits the shift moved into it.
  const uint64_t v_top = v[n - 1];
  const uint64_t v_next = v[n - 2];
  for (size_t j = (2 * N) - n + 1; j-- > 0;) {
    // D3. Estimate the digit as floor((u[j + n] * b + u[j + n - 1]) / v_top),
    // with rhat what that leaves. Knuth's estimate is b or b + 1 when the two
    // top limbs are equal, and his test then takes it down to b - 1, so start
    // there. That keeps qhat in a limb. rhat is kept in one too: the test only
    // runs while rhat is below b.
    uint64_t qhat = ~uint64_t{0};
    uint64_t rhat = u[j + n - 1] + v_top;
    bool rhat_fits = rhat >= v_top;
    if (u[j + n] != v_top) {
      qhat = limb_div(u[j + n], u[j + n - 1], v_top);
      rhat = u[j + n - 1] - (qhat * v_top);
      rhat_fits = true;
    }
    // The test: while qhat * v_next > rhat * b + u[j + n - 2], qhat is too
    // large. Each pass adds v_top, at least b / 2, to rhat, so there are at
    // most two. After it qhat is the digit or one above it.
    while (rhat_fits) {
      const uint128_t product = uint128_t{qhat} * v_next;
      const auto product_high = static_cast<uint64_t>(product >> 64);
      const auto product_low = static_cast<uint64_t>(product);
      if (product_high < rhat ||
          (product_high == rhat && product_low <= u[j + n - 2]))
        break;
      --qhat;
      rhat += v_top;
      rhat_fits = rhat >= v_top;
    }

    // D4. Multiply and subtract: u[j .. j + n] -= qhat * v.
    uint64_t carry = 0;
    uint64_t borrow = 0;
    for (size_t i = 0; i < n; ++i) {
      // Fits 128 bits: (2^64 - 1)^2 + (2^64 - 1) < 2^128.
      const uint128_t product = (uint128_t{qhat} * v[i]) + carry;
      carry = static_cast<uint64_t>(product >> 64);
      const auto low = static_cast<uint64_t>(product);
      // u[j + i] - low - borrow. It borrows if either subtraction wraps.
      const uint64_t difference = u[j + i] - low;
      const auto wrapped = static_cast<uint64_t>(u[j + i] < low);
      u[j + i] = difference - borrow;
      borrow = wrapped | static_cast<uint64_t>(difference < borrow);
    }
    // The top limb, less what the products carried out.
    const uint64_t difference = u[j + n] - carry;
    const auto wrapped = static_cast<uint64_t>(u[j + n] < carry);
    u[j + n] = difference - borrow;
    borrow = wrapped | static_cast<uint64_t>(difference < borrow);

    // D5, D6. A borrow says qhat was one above the digit, which happens for
    // about 2 digits in b: add the divisor back. The carry out of that cancels
    // the borrow.
    if (borrow != 0) {
      --qhat;
      carry = 0;
      for (size_t i = 0; i < n; ++i) {
        const uint128_t sum = uint128_t{u[j + i]} + v[i] + carry;
        u[j + i] = static_cast<uint64_t>(sum);
        carry = static_cast<uint64_t>(sum >> 64);
      }
      u[j + n] += carry;
    }
    result.quotient[j] = qhat;
  }

  // D8. Unnormalize: the remainder is u[0 .. n), shifted back. u[n] is zero.
  for (size_t i = 0; i < n; ++i)
    result.remainder[i] = (u[i] >> shift) | ((u[i + 1] << 1) << (63 - shift));
  return result;
}

template<size_t N> struct LimbsMulDivRem {
  Limbs<N> quotient_lo;
  Limbs<N> quotient_hi;
  Limbs<N> remainder;

  constexpr bool operator==(const LimbsMulDivRem &) const = default;
};

// floor(a * b / divisor) as its low and high N limbs, and a * b mod divisor,
// or nullopt for a zero divisor. The product is not truncated, so
//   a * b == (quotient_hi * 2^(64 * N) + quotient_lo) * divisor + remainder
// with remainder < divisor.
template<size_t N>
constexpr std::optional<LimbsMulDivRem<N>>
limbs_mul_div_rem(const Limbs<N> &a, const Limbs<N> &b, const Limbs<N> &divisor)
{
  const auto divided = limbs_div_rem<N>(limbs_mul(a, b), divisor);
  if (!divided)
    return std::nullopt;
  LimbsMulDivRem<N> result{};
  for (size_t i = 0; i < N; ++i) {
    result.quotient_lo[i] = divided->quotient[i];
    result.quotient_hi[i] = divided->quotient[i + N];
  }
  result.remainder = divided->remainder;
  return result;
}

// Square roots: floor(sqrt(value)).
//
// Each step is Zimmermann's SqrtRem ("Karatsuba Square Root", INRIA RR-3805).
// For n = a3*b^3 + a2*b^2 + a1*b + a0 with a3 >= b/4, in the paper's names:
//
//   (s', r') = SqrtRem(a3*b + a2)
//   (q, u)   = DivRem(r'*b + a1, 2*s')
//   s = s'*b + q
//   r = u*b + a0 - q^2
//   if r < 0: s = s - 1
//
// r is n - s^2, so the last line takes one off s when its square passes n.
// The code keeps the root and not the remainder, and differs in three ways,
// none of which changes s:
//
//   - r' is recomputed as a3*b + a2 - s'^2, and r < 0 is tested as s^2 > n.
//   - r'*b + a1 can need one bit more than a3*b + a2 has. q is taken as
//     floor(floor((r'*b + a1) / 2) / s'), which is the same number.
//   - q can come out as b. The paper leaves that to the last line, which then
//     gives s'*b + b - 1. Here q has to fit below s', so it is cut to b - 1.
//
// a3 >= b/4 says the top two bits of n are not both zero. The same then holds
// for a3*b + a2, so the steps run from the top eight bits of the value up,
// each doubling how much of it s is the root of.

// floor(sqrt(n)) for an n whose top two bits are not both zero.
constexpr uint64_t
isqrt_normalized_u128(uint128_t n)
{
  // The top eight bits are 64 to 255, and their root 8 to 15. Its top bit is
  // known, and each of the other three stays if the square still fits.
  const auto top8 = static_cast<uint64_t>(n >> 120);
  uint64_t root = 8;
  for (uint64_t bit = 4; bit != 0; bit >>= 1) {
    const uint64_t candidate = root | bit;
    if (candidate * candidate <= top8)
      root = candidate;
  }
  // Each pass is one step. part, the top bits of n, is the step's n, b is
  // 2^quarter, and root goes from s' to s.
  for (uint32_t bits = 16; bits <= 128; bits *= 2) {
    const uint32_t quarter = bits / 4;
    const uint64_t mask = (uint64_t{1} << quarter) - 1;
    const uint128_t part = n >> (128 - bits);
    // a3*b + a2, r' and a1.
    const auto high = static_cast<uint64_t>(part >> (2 * quarter));
    const uint64_t rest = high - (root * root);
    const uint64_t a1 = static_cast<uint64_t>(part >> quarter) & mask;
    // q. Halved, r'*b + a1 is r' * (b / 2) + floor(a1 / 2). r' is below 2b, so
    // that is below b^2, and its two parts share no bits.
    const uint64_t low =
        std::min(((rest << (quarter - 1)) | (a1 >> 1)) / root, mask);
    root = (root << quarter) | low;
    // if r < 0. Whether it holds follows no pattern, so the one comes off by
    // arithmetic and not through a branch.
    root -= static_cast<uint64_t>(uint128_t{root} * root > part);
  }
  return root;
}

// The root of a 256-bit value is below 2^128, so it is two limbs.
constexpr Limbs<2>
limbs_isqrt(const Limbs<4> &value)
{
  int zeros = 0;
  size_t limbs = value.size();
  while (limbs > 0 && value[limbs - 1] == 0) {
    --limbs;
    zeros += 64;
  }
  if (limbs == 0)
    return {};
  zeros += std::countl_zero(value[limbs - 1]);
  const int shift = zeros & ~1;
  const Limbs<4> n = limbs_shl(value, static_cast<size_t>(shift));
  // The last step, with b = 2^64: top is s', rest r' and low q.
  const uint128_t high = (uint128_t{n[3]} << 64) | n[2];
  const uint64_t top = isqrt_normalized_u128(high);
  const uint128_t rest = high - (uint128_t{top} * top);
  // (r'*b + a1) / 2, which is below b^2.
  const uint128_t dividend = (rest << 63) | (n[1] >> 1);
  // q is b when the top half of the dividend reaches s'. It is cut to b - 1.
  const uint64_t low = static_cast<uint64_t>(dividend >> 64) >= top
                           ? ~uint64_t{0}
                           : static_cast<uint64_t>(dividend / top);
  Limbs<2> root = {low, top};
  // if r < 0. q is then above zero, since s'*b squares to no more than n, so
  // nothing is borrowed from s'.
  if (limbs_less(n, limbs_mul(root, root)))
    --root[0];
  const uint128_t unshifted =
      ((uint128_t{root[1]} << 64) | root[0]) >> (shift / 2);
  return {static_cast<uint64_t>(unshifted),
          static_cast<uint64_t>(unshifted >> 64)};
}

// The value of Bytes bytes, least significant first. Each eight are a limb as
// a little-endian host stores it, so there the bytes are the limbs.
template<size_t Bytes> requires (Bytes % 8 == 0)
constexpr Limbs<Bytes / 8>
limbs_from_le_bytes(const std::array<uint8_t, Bytes> &bytes)
{
  auto limbs = std::bit_cast<Limbs<Bytes / 8>>(bytes);
  for (uint64_t &limb : limbs)
    limb = le_to_cpu(limb);
  return limbs;
}

// The reverse: a value as 8 * N bytes, least significant first.
template<size_t N>
constexpr std::array<uint8_t, 8 * N>
limbs_to_le_bytes(Limbs<N> limbs)
{
  for (uint64_t &limb : limbs)
    limb = cpu_to_le(limb);
  return std::bit_cast<std::array<uint8_t, 8 * N>>(limbs);
}

constexpr Limbs<2>
limbs_from_u128(uint128_t value)
{
  return {static_cast<uint64_t>(value), static_cast<uint64_t>(value >> 64)};
}

constexpr uint128_t
limbs_to_u128(const Limbs<2> &limbs)
{
  return (uint128_t{limbs[1]} << 64) | limbs[0];
}

} // namespace hogd
