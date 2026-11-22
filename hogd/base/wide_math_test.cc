#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/base/wide_math.h"

// Checked four independent ways: tables computed with Python, Ref below, the
// compiler's 128-bit arithmetic, and a root found one bit at a time.
//
// Division needs more than random operands. Step D6 corrects its digit
// estimate for about 2 digits in 2^64, and the top limbs are equal about as
// rarely, so the tables and the constructed dividends are what reach those.

// Every optional read below follows an ASSERT_TRUE(has_value()), which the
// check does not see through.
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace hogd {
namespace {

constexpr uint64_t kMaxLimb = UINT64_MAX;
constexpr uint64_t kTopBit = uint64_t{1} << 63;

// A deliberately naive unsigned integer: 32-bit words, one bit at a time. 1024
// bits hold anything a test forms from a 512-bit quotient and a 256-bit
// divisor.
class Ref {
public:
  static constexpr size_t kWords = 32;

  Ref() = default;

  template<size_t N> explicit Ref(const Limbs<N> &limbs)
  {
    for (size_t i = 0; i < N; ++i) {
      words_[2 * i] = static_cast<uint32_t>(limbs[i]);
      words_[(2 * i) + 1] = static_cast<uint32_t>(limbs[i] >> 32);
    }
  }

  // Whether the value is below 2^(64 * limbs).
  bool fits(size_t limbs) const
  {
    for (size_t i = 2 * limbs; i < kWords; ++i) {
      if (words_[i] != 0)
        return false;
    }
    return true;
  }

  // The low N limbs.
  template<size_t N> Limbs<N> limbs() const
  {
    Limbs<N> out{};
    for (size_t i = 0; i < N; ++i)
      out[i] = words_[2 * i] | (uint64_t{words_[(2 * i) + 1]} << 32);
    return out;
  }

  bool bit(size_t i) const { return ((words_[i / 32] >> (i % 32)) & 1) != 0; }
  void set_bit(size_t i) { words_[i / 32] |= uint32_t{1} << (i % 32); }

  size_t bit_length() const
  {
    for (size_t i = 32 * kWords; i-- > 0;) {
      if (bit(i))
        return i + 1;
    }
    return 0;
  }

  void twice()
  {
    uint32_t carry = 0;
    for (uint32_t &word : words_) {
      const uint32_t next = word >> 31;
      word = (word << 1) | carry;
      carry = next;
    }
  }

  void add(const Ref &other)
  {
    uint64_t carry = 0;
    for (size_t i = 0; i < kWords; ++i) {
      const uint64_t sum = uint64_t{words_[i]} + other.words_[i] + carry;
      words_[i] = static_cast<uint32_t>(sum);
      carry = sum >> 32;
    }
  }

  // Requires other <= *this.
  void sub(const Ref &other)
  {
    uint64_t borrow = 0;
    for (size_t i = 0; i < kWords; ++i) {
      const uint64_t difference =
          uint64_t{words_[i]} - other.words_[i] - borrow;
      words_[i] = static_cast<uint32_t>(difference);
      borrow = (difference >> 32) & 1;
    }
  }

  bool operator==(const Ref &) const = default;

  friend bool operator<(const Ref &a, const Ref &b)
  {
    for (size_t i = kWords; i-- > 0;) {
      if (a.words_[i] != b.words_[i])
        return a.words_[i] < b.words_[i];
    }
    return false;
  }

private:
  std::array<uint32_t, kWords> words_{};
};

// Shift and add.
Ref
ref_mul(const Ref &a, const Ref &b)
{
  Ref product;
  for (size_t i = b.bit_length(); i-- > 0;) {
    product.twice();
    if (b.bit(i))
      product.add(a);
  }
  return product;
}

// Restoring long division, one bit of the dividend at a time.
std::pair<Ref, Ref>
ref_div_rem(const Ref &dividend, const Ref &divisor)
{
  Ref quotient;
  Ref remainder;
  for (size_t i = dividend.bit_length(); i-- > 0;) {
    remainder.twice();
    if (dividend.bit(i))
      remainder.set_bit(0);
    if (!(remainder < divisor)) {
      remainder.sub(divisor);
      quotient.set_bit(i);
    }
  }
  return {quotient, remainder};
}

template<size_t N>
Limbs<2 * N>
concat(const Limbs<N> &lo, const Limbs<N> &hi)
{
  Limbs<2 * N> out{};
  for (size_t i = 0; i < N; ++i) {
    out[i] = lo[i];
    out[i + N] = hi[i];
  }
  return out;
}

// The square root as wide_math.h first had it: one bit of the root at a time
// from the top, keeping a bit when the square still fits. Slow, and short
// enough to be right by inspection.
Limbs<2>
bit_by_bit_isqrt(const Limbs<4> &value)
{
  Limbs<2> root{};
  for (size_t bit = 128; bit-- > 0;) {
    Limbs<2> candidate = root;
    candidate[bit / 64] |= uint64_t{1} << (bit % 64);
    if (!limbs_less(value, limbs_mul(candidate, candidate)))
      root = candidate;
  }
  return root;
}

template<size_t N>
Limbs<N>
from_u64(uint64_t value)
{
  Limbs<N> out{};
  out[0] = value;
  return out;
}

template<size_t N>
std::string
hex(const Limbs<N> &value)
{
  std::string out = "0x";
  for (size_t i = N; i-- > 0;)
    out += std::format("{:016x}", value[i]);
  return out;
}

// Exactly `bits` significant bits: the top one set, the rest random.
template<size_t N>
Limbs<N>
random_bits(std::mt19937_64 &rng, size_t bits)
{
  Limbs<N> out{};
  if (bits == 0)
    return out;
  const size_t top = (bits - 1) / 64;
  const size_t top_bit = (bits - 1) % 64;
  for (size_t i = 0; i <= top; ++i)
    out[i] = rng();
  out[top] &= (uint64_t{2} << top_bit) - 1;
  out[top] |= uint64_t{1} << top_bit;
  return out;
}

// Limbs at and next to the values where carries, borrows and digit estimates
// change behaviour.
template<size_t N>
Limbs<N>
random_edge_limbs(std::mt19937_64 &rng)
{
  constexpr std::array<uint64_t, 10> kEdges = {
      0,           1,       2,           0xffffffff,   0x100000000,
      kTopBit - 1, kTopBit, kTopBit + 1, kMaxLimb - 1, kMaxLimb,
  };
  Limbs<N> out{};
  for (uint64_t &limb : out) {
    // Two picks in twelve are an ordinary random limb.
    const uint64_t pick = rng() % (kEdges.size() + 2);
    limb = pick < kEdges.size() ? kEdges[pick] : rng();
  }
  return out;
}

// 2^k - 1, 2^k and 2^k + 1 for the k next to each limb boundary, with 0, 1, 2
// and the two largest values: where a limb fills, empties or carries.
template<size_t N>
std::vector<Limbs<N>>
boundary_values()
{
  const Ref one(from_u64<N>(1));
  std::vector<Limbs<N>> values = {from_u64<N>(0), from_u64<N>(1),
                                  from_u64<N>(2)};
  // k = 63, 64, 127, 128, ...
  for (size_t k = 63; k < 64 * N; k += (k % 64 == 63 ? 1 : 63)) {
    Ref power;
    power.set_bit(k);
    Ref below = power;
    below.sub(one);
    Ref above = power;
    above.add(one);
    values.push_back(below.limbs<N>());
    values.push_back(power.limbs<N>());
    values.push_back(above.limbs<N>());
  }
  Limbs<N> largest{};
  largest.fill(kMaxLimb);
  values.push_back(largest);
  largest[0] = kMaxLimb - 1;
  values.push_back(largest);
  return values;
}

// The quotient and remainder of a division are the only pair with
// dividend == quotient * divisor + remainder and remainder < divisor, so this
// is a complete check that needs only the reference's multiply.
template<size_t N>
void
expect_divides(const Ref &dividend, const Limbs<N> &divisor,
               const Limbs<2 * N> &quotient, const Limbs<N> &remainder)
{
  Ref rebuilt = ref_mul(Ref(quotient), Ref(divisor));
  rebuilt.add(Ref(remainder));
  EXPECT_EQ(rebuilt, dividend)
      << "divisor " << hex(divisor) << " quotient " << hex(quotient)
      << " remainder " << hex(remainder);
  EXPECT_TRUE(Ref(remainder) < Ref(divisor))
      << "divisor " << hex(divisor) << " remainder " << hex(remainder);
}

template<size_t N>
void
expect_mul_div_rem(const Limbs<N> &a, const Limbs<N> &b,
                   const Limbs<N> &divisor)
{
  const auto got = limbs_mul_div_rem(a, b, divisor);
  ASSERT_TRUE(got.has_value());
  SCOPED_TRACE("a " + hex(a) + " b " + hex(b));
  expect_divides(ref_mul(Ref(a), Ref(b)), divisor,
                 concat(got->quotient_lo, got->quotient_hi), got->remainder);
}

// The compiler evaluates these, and it rejects undefined behaviour in a
// constant expression: an oversized shift, an index out of range or a read of
// an uninitialized limb on any path they take would not compile. The division
// ones take the one-limb path, both shift cases, and steps D3 to D6.
static_assert(limbs_mul<2>({kMaxLimb, kMaxLimb}, {kMaxLimb, kMaxLimb}) ==
              Limbs<4>{1, 0, kMaxLimb - 1, kMaxLimb});
static_assert(limbs_div_rem<2>({7, 0, 0, 0}, {0, 0}) == std::nullopt);
// Dividing by 16 is a shift by four bits.
static_assert(limbs_div_rem<2>({1, 2, 3, 4}, {16, 0}) ==
              LimbsDivRem<2>{.quotient = {0x2000000000000000,
                                          0x3000000000000000,
                                          0x4000000000000000, 0},
                             .remainder = {1, 0}});
static_assert(limbs_div_rem<2>({0, 0xfffffffe, 0x80000000, 0},
                               {0xffffffff, 0x80000000}) ==
              LimbsDivRem<2>{.quotient = {kMaxLimb, 0, 0, 0},
                             .remainder = {0xffffffff, 0x7fffffff}});
static_assert(limbs_div_rem<4>({0, 0, 0, kTopBit, 1, 0, 0, 0},
                               {1, 0, 0, kTopBit}) ==
              LimbsDivRem<4>{.quotient = {2, 0, 0, 0, 0, 0, 0, 0},
                             .remainder = {kMaxLimb - 1, kMaxLimb, kMaxLimb,
                                           kTopBit - 1}});
static_assert(limbs_div_rem<4>({0, 0, 0, 0xc000000000000000, 0, 0, 0, 0},
                               {1, 0, 0, 0x4000000000000000}) ==
              LimbsDivRem<4>{.quotient = {2, 0, 0, 0, 0, 0, 0, 0},
                             .remainder = {kMaxLimb - 1, kMaxLimb, kMaxLimb,
                                           0x3fffffffffffffff}});
static_assert(limbs_mul_div_rem<4>({kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
                                   {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
                                   {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb}) ==
              LimbsMulDivRem<4>{.quotient_lo = {kMaxLimb, kMaxLimb, kMaxLimb,
                                                kMaxLimb},
                                .quotient_hi = {0, 0, 0, 0},
                                .remainder = {0, 0, 0, 0}});
static_assert(limbs_isqrt({kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb}) ==
              Limbs<2>{kMaxLimb, kMaxLimb});
static_assert(limbs_isqrt({0, 0, 1, 0}) == Limbs<2>{0, 1});
static_assert(limbs_isqrt({kMaxLimb, kMaxLimb, 0, 0}) == Limbs<2>{kMaxLimb, 0});
static_assert(limbs_isqrt({}) == Limbs<2>{});
static_assert(limbs_isqrt({3, 0, 0, 0}) == Limbs<2>{1, 0});
static_assert(limbs_shl<2>({1, 0}, 127) == Limbs<2>{0, kTopBit});
static_assert(limbs_from_le_bytes(std::array<uint8_t, 16>{1, 2, 3, 4, 5, 6, 7,
                                                          8, 9}) ==
              Limbs<2>{0x0807060504030201, 9});
static_assert(limbs_to_le_bytes<2>({0x0807060504030201, 9}) ==
              std::array<uint8_t, 16>{1, 2, 3, 4, 5, 6, 7, 8, 9});
static_assert(isqrt_normalized_u128(uint128_t{1} << 126) == kTopBit);
static_assert(isqrt_normalized_u128(uint128_t{1} << 127) == 0xb504f333f9de6484);
static_assert(isqrt_normalized_u128(~uint128_t{0}) == kMaxLimb);

TEST(WideMathTest, ByteAndWordOrder)
{
  std::array<uint8_t, 32> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<uint8_t>(i + 1);
  EXPECT_EQ(limbs_from_le_bytes(bytes),
            (Limbs<4>{0x0807060504030201, 0x100f0e0d0c0b0a09,
                      0x1817161514131211, 0x201f1e1d1c1b1a19}));

  // One is a single 1 in byte 0, and 2^255 a single 0x80 in byte 31.
  std::array<uint8_t, 32> one{};
  one[0] = 1;
  EXPECT_EQ(limbs_from_le_bytes(one), from_u64<4>(1));
  std::array<uint8_t, 32> top{};
  top[31] = 0x80;
  EXPECT_EQ(limbs_from_le_bytes(top), (Limbs<4>{0, 0, 0, kTopBit}));

  // And back: the bytes of a value are the ones it was read from.
  EXPECT_EQ(limbs_to_le_bytes(limbs_from_le_bytes(bytes)), bytes);
  EXPECT_EQ(limbs_to_le_bytes<4>({0, 0, 0, kTopBit}), top);

  const uint128_t value =
      (uint128_t{0x0123456789abcdef} << 64) | 0xfedcba9876543210;
  EXPECT_TRUE(limbs_to_u128({0xfedcba9876543210, 0x0123456789abcdef}) == value);
  EXPECT_EQ(limbs_from_u128(value),
            (Limbs<2>{0xfedcba9876543210, 0x0123456789abcdef}));
}

TEST(WideMathTest, LessIsNumericOrder)
{
  // Limb 0 is the least significant, so these differ from std::array's order.
  EXPECT_TRUE(limbs_less<2>({2, 1}, {1, 2}));
  EXPECT_FALSE(limbs_less<2>({1, 2}, {2, 1}));
  EXPECT_FALSE(limbs_less<2>({5, 5}, {5, 5}));
  EXPECT_TRUE(limbs_less<2>({kMaxLimb, 0}, {0, 1}));
}

TEST(WideMathTest, OneLimbMulMatchesNativeArithmetic)
{
  std::mt19937_64 rng(1);
  for (int i = 0; i < 20000; ++i) {
    const uint64_t a = random_edge_limbs<1>(rng)[0];
    const uint64_t b = random_edge_limbs<1>(rng)[0];
    const uint128_t product = uint128_t{a} * b;
    ASSERT_TRUE(limbs_to_u128(limbs_mul<1>({a}, {b})) == product);
  }
}

TEST(WideMathTest, MulMatchesReference)
{
  std::mt19937_64 rng(2);
  for (size_t bits = 0; bits <= 256; ++bits) {
    const Limbs<4> a = random_bits<4>(rng, bits);
    const size_t other_bits = rng() % 257;
    for (const Limbs<4> &b :
         {random_bits<4>(rng, other_bits), random_edge_limbs<4>(rng),
          Limbs<4>{kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb}}) {
      const Limbs<8> product = limbs_mul(a, b);
      ASSERT_EQ(Ref(product), ref_mul(Ref(a), Ref(b)))
          << hex(a) << " * " << hex(b);
      ASSERT_EQ(limbs_mul(b, a), product);
    }
  }
  for (size_t bits = 0; bits <= 128; ++bits) {
    const Limbs<2> a = random_bits<2>(rng, bits);
    const Limbs<2> b = random_edge_limbs<2>(rng);
    ASSERT_EQ(Ref(limbs_mul(a, b)), ref_mul(Ref(a), Ref(b)))
        << hex(a) << " * " << hex(b);
  }
}

TEST(WideMathTest, ZeroDivisorHasNoResult)
{
  EXPECT_EQ(limbs_div_rem<2>({1, 2, 3, 4}, {0, 0}), std::nullopt);
  EXPECT_EQ(limbs_div_rem<4>({}, {}), std::nullopt);
  EXPECT_EQ(limbs_mul_div_rem<2>({1, 2}, {3, 4}, {0, 0}), std::nullopt);
  EXPECT_EQ(limbs_mul_div_rem<4>({1, 2, 3, 4}, {5, 6, 7, 8}, {}), std::nullopt);
  // Zero only when every limb is.
  for (size_t i = 0; i < 4; ++i) {
    Limbs<4> divisor{};
    divisor[i] = 1;
    EXPECT_TRUE(
        limbs_mul_div_rem<4>({1, 2, 3, 4}, {5, 6, 7, 8}, divisor).has_value());
  }
}

TEST(WideMathTest, OneLimbMulDivRemMatchesNativeArithmetic)
{
  std::mt19937_64 rng(7);
  for (int i = 0; i < 20000; ++i) {
    const uint64_t a = random_edge_limbs<1>(rng)[0];
    const uint64_t b = random_edge_limbs<1>(rng)[0];
    const uint64_t d = random_edge_limbs<1>(rng)[0];
    const auto got = limbs_mul_div_rem<1>({a}, {b}, {d});
    if (d == 0) {
      ASSERT_FALSE(got.has_value());
      continue;
    }
    ASSERT_TRUE(got.has_value());
    const uint128_t product = uint128_t{a} * b;
    const uint128_t quotient = product / d;
    ASSERT_EQ(got->quotient_lo[0], static_cast<uint64_t>(quotient));
    ASSERT_EQ(got->quotient_hi[0], static_cast<uint64_t>(quotient >> 64));
    ASSERT_EQ(got->remainder[0], static_cast<uint64_t>(product % d));
  }
}

struct DivRemVector {
  Limbs<8> dividend;
  Limbs<4> divisor;
  Limbs<8> quotient;
  Limbs<4> remainder;
};

// Expected values computed with Python integers, and the step each row is
// marked as reaching confirmed with an instrumented build. The first six are
// the usual 32-bit shapes for the add-back and for a first estimate of b + 1,
// each limb pattern widened to 64 bits; the later ones have a remainder one or
// two below the divisor.
constexpr DivRemVector kDivRemVectors[] = {
    // Add-back, three-limb divisor; reaches D6.
    {.dividend = {3, 0, kTopBit, 0, 0, 0, 0, 0},
     .divisor = {1, 0, 0x2000000000000000, 0},
     .quotient = {3, 0, 0, 0, 0, 0, 0, 0},
     .remainder = {0, 0, 0x2000000000000000, 0}},
    // Add-back, three-limb divisor, half-limb patterns; reaches D6.
    {.dividend = {3, 0, 0x80000000, 0, 0, 0, 0, 0},
     .divisor = {1, 0, 0x20000000, 0},
     .quotient = {3, 0, 0, 0, 0, 0, 0, 0},
     .remainder = {0, 0, 0x20000000, 0}},
    // Add-back in the top digit; reaches D6.
    {.dividend = {0, 0, 0x80000000, 0x7fffffff, 0, 0, 0, 0},
     .divisor = {1, 0, 0x80000000, 0},
     .quotient = {0xfffffffe00000000, 0, 0, 0, 0, 0, 0, 0},
     .remainder = {0x200000000, kMaxLimb, 0x7fffffff, 0}},
    // Add-back; reaches D6, estimate starts at or above b.
    {.dividend = {0, 0xfffffffe, 0, 0x80000000, 0, 0, 0, 0},
     .divisor = {0xffffffff, 0, 0x80000000, 0},
     .quotient = {kMaxLimb, 0, 0, 0, 0, 0, 0, 0},
     .remainder = {0xffffffff, kMaxLimb, 0x7fffffff, 0}},
    // Add-back; reaches D6, estimate starts at or above b.
    {.dividend = {0, kMaxLimb - 1, 0, kTopBit, 0, 0, 0, 0},
     .divisor = {kMaxLimb, 0, kTopBit, 0},
     .quotient = {kMaxLimb, 0, 0, 0, 0, 0, 0, 0},
     .remainder = {kMaxLimb, kMaxLimb, kTopBit - 1, 0}},
    // First estimate is b + 1; estimate starts at or above b.
    {.dividend = {0, 0xfffffffe, 0x80000000, 0, 0, 0, 0, 0},
     .divisor = {0xffffffff, 0x80000000, 0, 0},
     .quotient = {kMaxLimb, 0, 0, 0, 0, 0, 0, 0},
     .remainder = {0xffffffff, 0x7fffffff, 0, 0}},
    // Add-back, four-limb divisor; reaches D6.
    {.dividend = {0, 0, 0, kTopBit, 1, 0, 0, 0},
     .divisor = {1, 0, 0, kTopBit},
     .quotient = {2, 0, 0, 0, 0, 0, 0, 0},
     .remainder = {kMaxLimb - 1, kMaxLimb, kMaxLimb, kTopBit - 1}},
    // Add-back, four-limb divisor, shifted; reaches D6.
    {.dividend = {0, 0, 0, 0xc000000000000000, 0, 0, 0, 0},
     .divisor = {1, 0, 0, 0x4000000000000000},
     .quotient = {2, 0, 0, 0, 0, 0, 0, 0},
     .remainder = {kMaxLimb - 1, kMaxLimb, kMaxLimb, 0x3fffffffffffffff}},
    // Add-back, four-limb divisor, top digit of eight; reaches D6, estimate
    // starts at or above b.
    {.dividend = {0, 0, 0, 0, 0, 0, 0, kTopBit},
     .divisor = {1, 0, 0, kTopBit},
     .quotient = {kMaxLimb - 1, kMaxLimb, kMaxLimb, kMaxLimb, 0, 0, 0, 0},
     .remainder = {2, 0, 0, 0}},
    // All ones over all ones.
    {.dividend = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb,
                  kMaxLimb, kMaxLimb},
     .divisor = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .quotient = {1, 0, 0, 0, 1, 0, 0, 0},
     .remainder = {0, 0, 0, 0}},
    // All ones over top bit.
    {.dividend = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb,
                  kMaxLimb, kMaxLimb},
     .divisor = {0, 0, 0, kTopBit},
     .quotient = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, 1, 0, 0, 0},
     .remainder = {kMaxLimb, kMaxLimb, kMaxLimb, kTopBit - 1}},
    // All ones over 2^192 + 1.
    {.dividend = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb,
                  kMaxLimb, kMaxLimb},
     .divisor = {1, 0, 0, 1},
     .quotient = {0, 0, kMaxLimb, kMaxLimb, kMaxLimb, 0, 0, 0},
     .remainder = {kMaxLimb, kMaxLimb, 0, 0}},
    // All ones over one limb.
    {.dividend = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb,
                  kMaxLimb, kMaxLimb},
     .divisor = {kMaxLimb, 0, 0, 0},
     .quotient = {1, 1, 1, 1, 1, 1, 1, 1},
     .remainder = {0, 0, 0, 0}},
    // Dividend below divisor.
    {.dividend = {kMaxLimb, kMaxLimb, kMaxLimb, kTopBit - 1, 0, 0, 0, 0},
     .divisor = {0, 0, 0, kTopBit},
     .quotient = {0, 0, 0, 0, 0, 0, 0, 0},
     .remainder = {kMaxLimb, kMaxLimb, kMaxLimb, kTopBit - 1}},
    // Dividend equals divisor.
    {.dividend = {0x3039, 0, 0, kTopBit, 0, 0, 0, 0},
     .divisor = {0x3039, 0, 0, kTopBit},
     .quotient = {1, 0, 0, 0, 0, 0, 0, 0},
     .remainder = {0, 0, 0, 0}},
    // Quotient * divisor + divisor - 1, 3-limb divisor; reaches D6.
    {.dividend = {0x16b81f78bab8ca77, 0x4f87fac9bb62bcda, 0xe843fa462932c6e6,
                  0x2c160c0e50bfee2c, 0x9725e5896ff8d703, 0xc6bef901a7825f0c,
                  0x6f902be769401335, 0x173c235c6008abac},
     .divisor = {0x95bd447e7184d454, 0x62f746b484e61ceb, 0xb80469b84b2d451c, 0},
     .quotient = {0x2399d29db61e6ba5, 0x1b2ba06b6ae12c97, 0xf5b19f71ec7ac8ad,
                  0x2248cd706ca4a1a9, 0x2052e52bdc36fc90, 0, 0, 0},
     .remainder = {0x95bd447e7184d453, 0x62f746b484e61ceb, 0xb80469b84b2d451c,
                   0}},
    // Quotient * divisor + divisor - 1, 4-limb divisor; reaches D6.
    {.dividend = {0x6b923bab9a6e5ac1, 0xfacea98fe8814fb9, 0xd1fc115f300438a8,
                  0xae1ab8878a49cd8f, 0x83d462ab7dd1773e, 0x806e3823679e3f34,
                  0x69c8a2c5687d47a6, 0x165432d63221f088},
     .divisor = {0x939ae232588efbce, 0xc85990d2e844c762, 0x5e571eaa66c164e7,
                 0x60ec6db25097c040},
     .quotient = {0x3f84c485e5c4176, 0x8c3f846cc6a942d3, 0x3da9d8c096d38ff5,
                  0x3af9f30c5a661477, 0, 0, 0, 0},
     .remainder = {0x939ae232588efbcd, 0xc85990d2e844c762, 0x5e571eaa66c164e7,
                   0x60ec6db25097c040}},
    // All-ones quotient, 2-limb divisor; estimate starts at or above b.
    {.dividend = {kMaxLimb - 1, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb,
                  kMaxLimb, 0x5eda7e46a386c4a6, 0xbce1cb3400e287d7},
     .divisor = {0x5eda7e46a386c4a7, 0xbce1cb3400e287d7, 0, 0},
     .quotient = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, 0,
                  0},
     .remainder = {0x5eda7e46a386c4a5, 0xbce1cb3400e287d7, 0, 0}},
    // All-ones quotient, 3-limb divisor; reaches D6, estimate starts at or
    // above b.
    {.dividend = {kMaxLimb - 1, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb,
                  0xbaeebb48936b7ac1, 0x7592b89ff910d68a, 0x8434d99c09ff7f0c},
     .divisor = {0xbaeebb48936b7ac2, 0x7592b89ff910d68a, 0x8434d99c09ff7f0c, 0},
     .quotient = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, 0, 0, 0},
     .remainder = {0xbaeebb48936b7ac0, 0x7592b89ff910d68a, 0x8434d99c09ff7f0c,
                   0}},
    // All-ones quotient, 4-limb divisor; reaches D6, estimate starts at or
    // above b.
    {.dividend = {kMaxLimb - 1, kMaxLimb, kMaxLimb, kMaxLimb,
                  0xa9cc2ff74eaf6d60, 0x1b2410a6f0ae1284, 0x944782d73418a2c5,
                  0xc074a6442ba33d67},
     .divisor = {0xa9cc2ff74eaf6d61, 0x1b2410a6f0ae1284, 0x944782d73418a2c5,
                 0xc074a6442ba33d67},
     .quotient = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb, 0, 0, 0, 0},
     .remainder = {0xa9cc2ff74eaf6d5f, 0x1b2410a6f0ae1284, 0x944782d73418a2c5,
                   0xc074a6442ba33d67}},
    // The top limb of the subtraction wraps with no borrow coming in, twice;
    // reaches D6.
    {.dividend = {0, kMaxLimb, kMaxLimb - 1, kTopBit + 1, 0, kTopBit, kMaxLimb,
                  kTopBit - 1},
     .divisor = {kTopBit + 1, kTopBit + 1, kTopBit, kTopBit},
     .quotient = {4, kMaxLimb, kMaxLimb - 1, kMaxLimb - 1, 0, 0, 0, 0},
     .remainder = {0xfffffffffffffffc, 0x7ffffffffffffff9, kMaxLimb, 2}},
    // The same once, three-limb divisor; reaches D6.
    {.dividend = {1, kMaxLimb, 1, 1, kTopBit + 1, kMaxLimb, kMaxLimb, kTopBit},
     .divisor = {kMaxLimb - 1, kTopBit + 1, kTopBit + 1, 0},
     .quotient = {0x2a, 0xffffffffffffffe4, 9, kMaxLimb, kMaxLimb - 1, 0, 0, 0},
     .remainder = {0x55, 0xffffffffffffff73, 0xe, 0}},
};

TEST(WideMathTest, DivRemVectors)
{
  for (const DivRemVector &v : kDivRemVectors) {
    const auto got = limbs_div_rem<4>(v.dividend, v.divisor);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->quotient, v.quotient)
        << hex(v.dividend) << " / " << hex(v.divisor);
    EXPECT_EQ(got->remainder, v.remainder)
        << hex(v.dividend) << " / " << hex(v.divisor);
  }
}

template<size_t N> struct MulDivRemVector {
  Limbs<N> a;
  Limbs<N> b;
  Limbs<N> divisor;
  LimbsMulDivRem<N> expected;
};

// Expected values computed with Python integers.
constexpr MulDivRemVector<4> kMulDivRem256Vectors[] = {
    // Small.
    {.a = {3, 0, 0, 0},
     .b = {5, 0, 0, 0},
     .divisor = {7, 0, 0, 0},
     .expected = {.quotient_lo = {2, 0, 0, 0},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {1, 0, 0, 0}}},
    // Divisor one: the quotient is the product.
    {.a = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .b = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .divisor = {1, 0, 0, 0},
     .expected = {.quotient_lo = {1, 0, 0, 0},
                  .quotient_hi = {kMaxLimb - 1, kMaxLimb, kMaxLimb, kMaxLimb},
                  .remainder = {0, 0, 0, 0}}},
    // Factor one: plain division.
    {.a = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .b = {1, 0, 0, 0},
     .divisor = {0xde0b6b3a7640000, 0, 0, 0},
     .expected = {.quotient_lo = {0x5afe688c928e1f21, 0x5fe645cc4873f9e6,
                                  0x725dd1d243aba0e7, 0x12},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0x81ad01a501bffff, 0, 0, 0}}},
    // All ones; reaches D6, estimate starts at or above b.
    {.a = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .b = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .divisor = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .expected = {.quotient_lo = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0, 0, 0, 0}}},
    // Quotient just overflows 256 bits.
    {.a = {0, 0, 0, kTopBit},
     .b = {2, 0, 0, 0},
     .divisor = {1, 0, 0, 0},
     .expected = {.quotient_lo = {0, 0, 0, 0},
                  .quotient_hi = {1, 0, 0, 0},
                  .remainder = {0, 0, 0, 0}}},
    // Quotient just fits 256 bits.
    {.a = {kMaxLimb, kMaxLimb, kMaxLimb, kTopBit - 1},
     .b = {2, 0, 0, 0},
     .divisor = {1, 0, 0, 0},
     .expected = {.quotient_lo = {kMaxLimb - 1, kMaxLimb, kMaxLimb, kMaxLimb},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0, 0, 0, 0}}},
    // Zero product.
    {.a = {0, 0, 0, 0},
     .b = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .divisor = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .expected = {.quotient_lo = {0, 0, 0, 0},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0, 0, 0, 0}}},
    // Top-bit divisor.
    {.a = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .b = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .divisor = {0, 0, 0, kTopBit},
     .expected = {.quotient_lo = {0xfffffffffffffffc, kMaxLimb, kMaxLimb,
                                  kMaxLimb},
                  .quotient_hi = {1, 0, 0, 0},
                  .remainder = {1, 0, 0, 0}}},
    // One-limb divisor.
    {.a = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .b = {kMaxLimb - 1, kMaxLimb, kMaxLimb, kMaxLimb},
     .divisor = {kMaxLimb, 0, 0, 0},
     .expected = {.quotient_lo = {kMaxLimb - 1, 0xfffffffffffffffd,
                                  0xfffffffffffffffd, 0xfffffffffffffffd},
                  .quotient_hi = {0, 1, 1, 1},
                  .remainder = {0, 0, 0, 0}}},
    // Two-limb divisor.
    {.a = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .b = {0xfffffffffffffffd, kMaxLimb, kMaxLimb, kMaxLimb},
     .divisor = {0, 1, 0, 0},
     .expected = {.quotient_lo = {0, 0, 0, 0xfffffffffffffffc},
                  .quotient_hi = {kMaxLimb, kMaxLimb, kMaxLimb, 0},
                  .remainder = {3, 0, 0, 0}}},
    // Two-limb divisor.
    {.a = {0x1000000000000000, 0xd762422c946590d9, 0x4f2726179a224501, 0x9f},
     .b = {0, 0x245689c107950240, 0x73c86d67c5faa71c, 0x172ebad6ddc},
     .divisor = {kMaxLimb, kTopBit - 1, 0, 0},
     .expected = {.quotient_lo = {0x4e4e938fdadf7d26, 0x63c8dcfac9f8db7d,
                                  0x23cf8231bb23862f, 0xd83b4c1b80b22ae9},
                  .quotient_hi = {0x1cda62055b2d9, 0, 0, 0},
                  .remainder = {0x4e4e938fdadf7d26, 0x63c8dcfac9f8db7d, 0, 0}}},
    // Three-limb divisor.
    {.a = {0xf8091a2b3c4d5e70, 0x7f6e5d4c3b2a1896, 0xfedcba9876543210,
           0x123456789abcdef0},
     .b = {0xffffffffffffcfc7, kMaxLimb, kMaxLimb, kMaxLimb},
     .divisor = {1, 0, 1, 0},
     .expected = {.quotient_lo = {0xe4b17e4b17e4997d, 0xb4e81b4e81b7f2e7,
                                  0xf92c5f92c5f928f1, 0x6d3a06d3a06d39a5},
                  .quotient_hi = {0xfedcba9876543210, 0x123456789abcdef0, 0, 0},
                  .remainder = {0x2c5f92c5f92c5f93, 0x3a06d3a06d4c44f9, 0, 0}}},
    // Fixed-point product, 18 decimals.
    {.a = {0x6420d97740000000, 0x5ce0e9a53831e393, 0, 0},
     .b = {0x34a2985c68000000, 0xbe371e1b7b6cb4, 0, 0},
     .divisor = {0xde0b6b3a7640000, 0, 0, 0},
     .expected = {.quotient_lo = {0x1431708000000000, 0x3ea99cf60a4910b4,
                                  0x4f9093bc3270bfd, 0},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0, 0, 0, 0}}},
    // Three-limb divisor through the product; reaches D6.
    {.a = {0xf92c5f92c5f92c65, 5, kTopBit, 1},
     .b = {1, 0, 0, 0},
     .divisor = {0xfedcba9876543211, 0, 0x4000000000000000, 0},
     .expected = {.quotient_lo = {5, 0, 0, 0},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0xfedcba9876543210, 0, 0x4000000000000000, 0}}},
    // Three-limb divisor through the product, shifted; reaches D6, estimate
    // starts at or above b.
    {.a = {0xf92c5f92c5f92c65, 5, kTopBit, 1},
     .b = {0, 1, 0, 0},
     .divisor = {0xfedcba9876543211, 0, 0x4000000000000000, 0},
     .expected = {.quotient_lo = {kMaxLimb, 5, 0, 0},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0xfedcba9876543211, kMaxLimb,
                                0x3fffffffffffffff, 0}}},
    // Four-limb divisor through the product; reaches D6.
    {.a = {0x72ea61d950c83b86, 0xfffffffffffffc1b, 0x3e8, 0x138d},
     .b = {1, 0, 0, 0},
     .divisor = {0x123456789abcdef, kMaxLimb, 0, 5},
     .expected = {.quotient_lo = {0x3e8, 0, 0, 0},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0x123456789abcdee, kMaxLimb, 0, 5}}},
    // Four-limb divisor through the product, shifted; reaches D6, estimate
    // starts at or above b.
    {.a = {0x91a2b3c4d5e6f77, 0xfffffffffffffff8, 7, 0x28},
     .b = {0, 1, 0, 0},
     .divisor = {0x123456789abcdef, kMaxLimb, 0, 5},
     .expected = {.quotient_lo = {kMaxLimb, 7, 0, 0},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0x123456789abcdef, kMaxLimb - 1, 0, 5}}},
    // Four-limb divisor, all-ones factors.
    {.a = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .b = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .divisor = {0x123456789abcdef, kMaxLimb, 0, 0x8000000000000001},
     .expected = {.quotient_lo = {0xfb72ea61d950c828, 3, 4, 0xfffffffffffffffc},
                  .quotient_hi = {1, 0, 0, 0},
                  .remainder = {0x92731f7552951aa9, 0xf6eb025f23716b1f, 0x1f,
                                0x91a2b3c4d5e6f94}}},
    // Four-limb divisor, squaring.
    {.a = {0x123456789abcdee, kMaxLimb, 0, 0x8000000000000001},
     .b = {0x123456789abcdee, kMaxLimb, 0, 0x8000000000000001},
     .divisor = {0x123456789abcdef, kMaxLimb, 0, 0x8000000000000001},
     .expected = {.quotient_lo = {0x123456789abcded, kMaxLimb, 0,
                                  0x8000000000000001},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {1, 0, 0, 0}}},
    // Random.
    {.a = {0xedeed7391ee6db62, 0x5d0991e00822e8e7, 0x37c3de4c02228d48,
           0x8b21ea852a848829},
     .b = {0x7ac0cddb0346ce06, 0xc199c0a98a624ac1, 0x627d239fdf8adf39,
           0x4ca8c324a8eb52f1},
     .divisor = {0xf7572dd88f18de53, 0x53623b53b93e295f, 0x9581467552608ba9,
                 0x999f49576886726},
     .expected = {.quotient_lo = {0xd5994b89a6b63fcb, 0x68a653cb5c7504e5,
                                  0x84f5fd006dead018, 0x56dbe8df6153acac},
                  .quotient_hi = {4, 0, 0, 0},
                  .remainder = {0x6d398389284d477b, 0x2a95bbefd0568e55,
                                0x2217a3649ff16943, 0x85ff2fcabe233d4}}},
    // Found by search: products that take the rare paths of the division
    // on most digits. The counts are from an instrumented build.
    // Adds the divisor back for 3 of 6 digits; three-limb divisor.
    {.a = {kMaxLimb, kMaxLimb - 1, kMaxLimb - 1, kMaxLimb - 1},
     .b = {kTopBit - 1, kTopBit - 1, 2, 0},
     .divisor = {kTopBit - 1, kTopBit - 1, 2, 0},
     .expected = {.quotient_lo = {kMaxLimb, kMaxLimb - 1, kMaxLimb - 1,
                                  kMaxLimb - 1},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0, 0, 0, 0}}},
    // Adds the divisor back for 3 of 5 digits; four-limb divisor.
    {.a = {2, kMaxLimb, 1, 2},
     .b = {0x100000000, kMaxLimb, kMaxLimb - 1, kMaxLimb - 1},
     .divisor = {3, kMaxLimb, 1, 2},
     .expected = {.quotient_lo = {0x8000000100000000, kMaxLimb - 1,
                                  kMaxLimb - 1, kMaxLimb - 1},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0x7fffffff00000000, 0x8000000000000002, 0, 2}}},
    // Takes 13 off the estimates of 7 digits; two-limb divisor.
    {.a = {kMaxLimb - 1, kTopBit, kTopBit, kTopBit},
     .b = {0xffffffff, 2, 1, kMaxLimb - 1},
     .divisor = {0xffffffff, 2, 0, 0},
     .expected = {.quotient_lo = {0xd6fbffffeb7dfffd, 0xdbefffffadf80000,
                                  0xefc00000b7e00001, 0xbeffffffdf7ffffe},
                  .quotient_hi = {0xfbfffffffe000000, 0xeffffffff7ffffff,
                                  0x3fffffffdfffffff, 0},
                  .remainder = {0xeb7e0000eb7dffff, 1, 0, 0}}},
    // Takes 12 off the estimates of 6 digits; three-limb divisor.
    {.a = {0xffffffff, 1, 0xe8ea0ff0ba84880e, 0xbbac448e01611c15},
     .b = {kTopBit, 0, kTopBit - 1, kMaxLimb - 1},
     .divisor = {kMaxLimb, 0xffffffff, 1, 0},
     .expected = {.quotient_lo = {0xe9611a4cc59827bd, 0xb95ca1ab5c91b5fc,
                                  0xc5f2d04aaef94208, 0xf3e0dbbd38ac13a0},
                  .quotient_hi = {0x89b2d1942ec00c58, 0xbbac448d45b4d787, 0, 0},
                  .remainder = {0x69611a4cc59827bd, 0x73c479eedc91b5fc, 0, 0}}},
    // Takes 10 off the estimates of 5 digits; four-limb divisor.
    {.a = {1, kMaxLimb - 1, 0xc1ba6a26af1f249f, kMaxLimb - 1},
     .b = {0xcaf97bdc90737abd, 0xf7013f3608eacd67, kMaxLimb, kMaxLimb - 1},
     .divisor = {kTopBit + 1, kMaxLimb - 1, 0xffffffff, 1},
     .expected = {.quotient_lo = {0xb4bcf65df844823c, 0xe6d672524e0e8895,
                                  0xc7e21a9311e98e2f, 0xc1ba6a27ed64ba7a},
                  .quotient_hi = {0xfffffffefffffffe, 0, 0, 0},
                  .remainder = {0x163c857e982ef881, 0x95346b78e5c12b2,
                                0xd02de75be6a8ee01, 0}}},
    // Equal top limbs for 5 of 7 digits; two-limb divisor.
    {.a = {1, kMaxLimb - 1, kMaxLimb, kMaxLimb},
     .b = {kMaxLimb, kMaxLimb, kMaxLimb - 1, kMaxLimb},
     .divisor = {kMaxLimb, kMaxLimb, 0, 0},
     .expected = {.quotient_lo = {kMaxLimb, kMaxLimb, kMaxLimb,
                                  0xfffffffffffffffd},
                  .quotient_hi = {kMaxLimb, kMaxLimb, 0, 0},
                  .remainder = {kMaxLimb - 1, 1, 0, 0}}},
    // Equal top limbs for 4 of 6 digits; three-limb divisor.
    {.a = {kMaxLimb, kMaxLimb - 1, kMaxLimb - 1, kMaxLimb - 1},
     .b = {kTopBit - 1, kTopBit - 1, 2, 0},
     .divisor = {kTopBit - 1, kTopBit - 1, 2, 0},
     .expected = {.quotient_lo = {kMaxLimb, kMaxLimb - 1, kMaxLimb - 1,
                                  kMaxLimb - 1},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0, 0, 0, 0}}},
    // Equal top limbs for 4 of 5 digits; four-limb divisor.
    {.a = {0x25140294fd8593f7, 1, 0xfc62e9528bb3c8ac, 0xb1bb684339f4b9f1},
     .b = {1, 0, kMaxLimb, kMaxLimb - 1},
     .divisor = {0x25140294fd8593f8, 1, 0xfc62e9528bb3c8ac, 0xb1bb684339f4b9f1},
     .expected = {.quotient_lo = {kMaxLimb, kMaxLimb, kMaxLimb - 1,
                                  kMaxLimb - 1},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {0x4a280529fb0b27ef, 2, 0xf8c5d2a517679159,
                                0x6376d08673e973e4}}},
    // The top limb of the subtraction wraps with no borrow coming in.
    {.a = {1, 2, 0, kTopBit - 1},
     .b = {1, 1, 2, kTopBit + 1},
     .divisor = {0, kMaxLimb - 1, 2, kTopBit + 1},
     .expected = {.quotient_lo = {0xfffffffffffffff3, 8, kMaxLimb,
                                  0x7ffffffffffffffe},
                  .quotient_hi = {0, 0, 0, 0},
                  .remainder = {1, 0xffffffffffffffe9, 0x3c,
                                0x7ffffffffffffff4}}},
};

constexpr MulDivRemVector<2> kMulDivRem128Vectors[] = {
    // Small.
    {.a = {3, 0},
     .b = {5, 0},
     .divisor = {7, 0},
     .expected = {.quotient_lo = {2, 0},
                  .quotient_hi = {0, 0},
                  .remainder = {1, 0}}},
    // Divisor one: the quotient is the product.
    {.a = {kMaxLimb, kMaxLimb},
     .b = {kMaxLimb, kMaxLimb},
     .divisor = {1, 0},
     .expected = {.quotient_lo = {1, 0},
                  .quotient_hi = {kMaxLimb - 1, kMaxLimb},
                  .remainder = {0, 0}}},
    // Factor one: plain division.
    {.a = {kMaxLimb, kMaxLimb},
     .b = {1, 0},
     .divisor = {0xde0b6b3a7640000, 0},
     .expected = {.quotient_lo = {0x725dd1d243aba0e7, 0x12},
                  .quotient_hi = {0, 0},
                  .remainder = {0x532df7974c3ffff, 0}}},
    // All ones; estimate starts at or above b.
    {.a = {kMaxLimb, kMaxLimb},
     .b = {kMaxLimb, kMaxLimb},
     .divisor = {kMaxLimb, kMaxLimb},
     .expected = {.quotient_lo = {kMaxLimb, kMaxLimb},
                  .quotient_hi = {0, 0},
                  .remainder = {0, 0}}},
    // Quotient just overflows 128 bits.
    {.a = {0, kTopBit},
     .b = {2, 0},
     .divisor = {1, 0},
     .expected = {.quotient_lo = {0, 0},
                  .quotient_hi = {1, 0},
                  .remainder = {0, 0}}},
    // Quotient just fits 128 bits.
    {.a = {kMaxLimb, kTopBit - 1},
     .b = {2, 0},
     .divisor = {1, 0},
     .expected = {.quotient_lo = {kMaxLimb - 1, kMaxLimb},
                  .quotient_hi = {0, 0},
                  .remainder = {0, 0}}},
    // Zero product.
    {.a = {0, 0},
     .b = {kMaxLimb, kMaxLimb},
     .divisor = {kMaxLimb, kMaxLimb},
     .expected = {.quotient_lo = {0, 0},
                  .quotient_hi = {0, 0},
                  .remainder = {0, 0}}},
    // Top-bit divisor.
    {.a = {kMaxLimb, kMaxLimb},
     .b = {kMaxLimb, kMaxLimb},
     .divisor = {0, kTopBit},
     .expected = {.quotient_lo = {0xfffffffffffffffc, kMaxLimb},
                  .quotient_hi = {1, 0},
                  .remainder = {1, 0}}},
    // One-limb divisor.
    {.a = {kMaxLimb, kMaxLimb},
     .b = {kMaxLimb - 1, kMaxLimb},
     .divisor = {kMaxLimb, 0},
     .expected = {.quotient_lo = {kMaxLimb - 1, 0xfffffffffffffffd},
                  .quotient_hi = {0, 1},
                  .remainder = {0, 0}}},
    // Smallest two-limb divisor.
    {.a = {kMaxLimb, kMaxLimb},
     .b = {0xfffffffffffffffd, kMaxLimb},
     .divisor = {0, 1},
     .expected = {.quotient_lo = {0, 0xfffffffffffffffc},
                  .quotient_hi = {kMaxLimb, 0},
                  .remainder = {3, 0}}},
    // First estimate is b + 1; estimate starts at or above b.
    {.a = {0xfffffffe, 0x80000000},
     .b = {0, 1},
     .divisor = {0xffffffff, 0x80000000},
     .expected = {.quotient_lo = {kMaxLimb, 0},
                  .quotient_hi = {0, 0},
                  .remainder = {0xffffffff, 0x7fffffff}}},
    // Token amount, 18 decimals.
    {.a = {0x4674edea40000000, 0xc9f2c9cd0},
     .b = {0x9fd0803ce8000000, 0x33b2e3c},
     .divisor = {0xde0b6b3a7640000, 0},
     .expected = {.quotient_lo = {0x5f65568000000000, 0xf050fe938943acc4},
                  .quotient_hi = {2, 0},
                  .remainder = {0, 0}}},
    // Random.
    {.a = {0xcb9b515fe4ec9f0d, 0x8eeb4467a5c46004},
     .b = {0x479dd02e5974cf6b, 0xdfed74b768c6714},
     .divisor = {0x4cca838c6e2d3c61, 0xf201da49cac858bb},
     .expected = {.quotient_lo = {0xfc7b38190cd2b216, 0x843defa800ebe0f},
                  .quotient_hi = {0, 0},
                  .remainder = {0x63031ff39af55b19, 0xcedd3fd7392f9355}}},
    // Found by search: products that take the rare paths of the division
    // on most digits. The counts are from an instrumented build.
    // Takes 5 off the estimates of 3 digits; two-limb divisor.
    {.a = {kTopBit - 1, kMaxLimb},
     .b = {kTopBit, kMaxLimb - 1},
     .divisor = {kMaxLimb - 1, 0x100000000},
     .expected = {.quotient_lo = {0xfffffffac0000003, 0xffffffff00000002},
                  .quotient_hi = {0xfffffffe, 0},
                  .remainder = {0x7ffffff580000006, 0x40000004}}},
    // Equal top limbs for 2 of 3 digits; two-limb divisor.
    {.a = {kMaxLimb, kMaxLimb},
     .b = {kTopBit - 1, 2},
     .divisor = {kTopBit - 1, 2},
     .expected = {.quotient_lo = {kMaxLimb, kMaxLimb},
                  .quotient_hi = {0, 0},
                  .remainder = {0, 0}}},
};

TEST(WideMathTest, MulDivRemVectors)
{
  for (const auto &v : kMulDivRem256Vectors) {
    EXPECT_EQ(limbs_mul_div_rem(v.a, v.b, v.divisor), v.expected)
        << hex(v.a) << " * " << hex(v.b) << " / " << hex(v.divisor);
    EXPECT_EQ(limbs_mul_div_rem(v.b, v.a, v.divisor), v.expected);
  }
  for (const auto &v : kMulDivRem128Vectors) {
    EXPECT_EQ(limbs_mul_div_rem(v.a, v.b, v.divisor), v.expected)
        << hex(v.a) << " * " << hex(v.b) << " / " << hex(v.divisor);
    EXPECT_EQ(limbs_mul_div_rem(v.b, v.a, v.divisor), v.expected);
    // The same operation at twice the width must agree.
    const auto wide =
        limbs_mul_div_rem<4>(concat<2>(v.a, {}), concat<2>(v.b, {}),
                             concat<2>(v.divisor, {}));
    ASSERT_TRUE(wide.has_value());
    EXPECT_EQ(wide->quotient_lo,
              concat(v.expected.quotient_lo, v.expected.quotient_hi));
    EXPECT_EQ(wide->quotient_hi, Limbs<4>{});
    EXPECT_EQ(wide->remainder, concat<2>(v.expected.remainder, {}));
  }
}

template<size_t N>
void
check_limb_boundaries()
{
  const std::vector<Limbs<N>> values = boundary_values<N>();
  for (size_t i = 0; i < values.size(); ++i) {
    for (size_t j = i; j < values.size(); ++j) {
      for (const Limbs<N> &divisor : values) {
        const auto got = limbs_mul_div_rem(values[i], values[j], divisor);
        if (divisor == Limbs<N>{}) {
          ASSERT_FALSE(got.has_value());
          continue;
        }
        ASSERT_NO_FATAL_FAILURE(
            expect_mul_div_rem(values[i], values[j], divisor));
        ASSERT_EQ(limbs_mul_div_rem(values[j], values[i], divisor), got);
        if (::testing::Test::HasFailure())
          return;
      }
    }
  }
}

TEST(WideMathTest, MulDivRemAtLimbBoundaries128)
{
  check_limb_boundaries<2>();
}

TEST(WideMathTest, MulDivRemAtLimbBoundaries256)
{
  check_limb_boundaries<4>();
}

// Every bit width of each operand in turn, the other two at random widths.
// Checked both by the identity and against the reference's long division.
template<size_t N>
void
check_every_bit_width(uint64_t seed)
{
  std::mt19937_64 rng(seed);
  const size_t max_bits = 64 * N;
  for (size_t bits = 0; bits <= max_bits; ++bits) {
    for (int position = 0; position < 3; ++position) {
      std::array<size_t, 3> widths = {rng() % (max_bits + 1),
                                      rng() % (max_bits + 1),
                                      1 + (rng() % max_bits)};
      if (position == 2 && bits == 0)
        continue;
      widths[position] = bits;
      const Limbs<N> a = random_bits<N>(rng, widths[0]);
      const Limbs<N> b = random_bits<N>(rng, widths[1]);
      const Limbs<N> divisor = random_bits<N>(rng, widths[2]);
      ASSERT_NO_FATAL_FAILURE(expect_mul_div_rem(a, b, divisor));

      const auto got = limbs_mul_div_rem(a, b, divisor);
      const auto [quotient, remainder] =
          ref_div_rem(ref_mul(Ref(a), Ref(b)), Ref(divisor));
      ASSERT_TRUE(got.has_value());
      ASSERT_EQ(Ref(concat(got->quotient_lo, got->quotient_hi)), quotient)
          << hex(a) << " * " << hex(b) << " / " << hex(divisor);
      ASSERT_EQ(Ref(got->remainder), remainder)
          << hex(a) << " * " << hex(b) << " / " << hex(divisor);
    }
  }
}

TEST(WideMathTest, MulDivRemEveryBitWidth128)
{
  check_every_bit_width<2>(3);
}

TEST(WideMathTest, MulDivRemEveryBitWidth256)
{
  check_every_bit_width<4>(4);
}

TEST(WideMathTest, MulDivRemEdgeLimbs)
{
  std::mt19937_64 rng(5);
  for (int i = 0; i < 1500; ++i) {
    const Limbs<4> a = random_edge_limbs<4>(rng);
    const Limbs<4> b = random_edge_limbs<4>(rng);
    const Limbs<4> divisor = random_edge_limbs<4>(rng);
    if (divisor != Limbs<4>{}) {
      ASSERT_NO_FATAL_FAILURE(expect_mul_div_rem(a, b, divisor));
    }

    const Limbs<2> narrow_a = random_edge_limbs<2>(rng);
    const Limbs<2> narrow_b = random_edge_limbs<2>(rng);
    const Limbs<2> narrow_divisor = random_edge_limbs<2>(rng);
    if (narrow_divisor != Limbs<2>{}) {
      ASSERT_NO_FATAL_FAILURE(
          expect_mul_div_rem(narrow_a, narrow_b, narrow_divisor));
    }
  }
}

// Builds dividend = quotient * divisor + remainder for chosen parts, so the
// expected answer is known without dividing. A remainder just below the
// divisor is what makes the digit estimate overshoot, and with a divisor of
// three or more limbs these dividends reach step D6 again and again; all-ones
// quotient limbs start the estimate at or above b.
TEST(WideMathTest, DivRemRecoversConstructedParts)
{
  std::mt19937_64 rng(6);
  for (int i = 0; i < 3000; ++i) {
    const size_t divisor_limbs = 1 + (rng() % 4);
    Limbs<4> divisor =
        i % 2 == 0 ? random_edge_limbs<4>(rng) : random_bits<4>(rng, 256);
    for (size_t limb = divisor_limbs; limb < 4; ++limb)
      divisor[limb] = 0;
    if (divisor[divisor_limbs - 1] == 0) {
      const uint64_t shift = rng() % 64;
      divisor[divisor_limbs - 1] = (rng() >> shift) | 1;
    }

    // The quotient may use every limb the divisor leaves free.
    Limbs<8> quotient{};
    for (size_t limb = 0; limb < 8 - divisor_limbs; ++limb)
      quotient[limb] = rng() % 3 == 0 ? kMaxLimb : rng();
    if (rng() % 4 == 0)
      quotient = concat(random_edge_limbs<4>(rng), Limbs<4>{});

    // The remainder is a small delta below the divisor, or sometimes the
    // delta itself; zero when the divisor is too small for either.
    const Ref delta(from_u64<4>(1 + (rng() % 3)));
    Ref remainder;
    if (delta < Ref(divisor)) {
      if (rng() % 4 != 0) {
        remainder = Ref(divisor);
        remainder.sub(delta);
      }
      else {
        remainder = delta;
      }
    }

    Ref dividend = ref_mul(Ref(quotient), Ref(divisor));
    dividend.add(remainder);
    ASSERT_TRUE(dividend.fits(8));

    const auto got = limbs_div_rem<4>(dividend.limbs<8>(), divisor);
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->quotient, quotient)
        << hex(dividend.limbs<8>()) << " / " << hex(divisor);
    ASSERT_EQ(got->remainder, remainder.limbs<4>())
        << hex(dividend.limbs<8>()) << " / " << hex(divisor);
  }
}

struct SqrtVector {
  Limbs<4> value;
  Limbs<2> root;
};

// Expected values computed with Python's math.isqrt.
constexpr SqrtVector kSqrtVectors[] = {
    {.value = {0, 0, 0, 0}, .root = {0, 0}},
    {.value = {1, 0, 0, 0}, .root = {1, 0}},
    {.value = {2, 0, 0, 0}, .root = {1, 0}},
    {.value = {3, 0, 0, 0}, .root = {1, 0}},
    {.value = {4, 0, 0, 0}, .root = {2, 0}},
    {.value = {8, 0, 0, 0}, .root = {2, 0}},
    {.value = {9, 0, 0, 0}, .root = {3, 0}},
    {.value = {0xf, 0, 0, 0}, .root = {3, 0}},
    {.value = {0x10, 0, 0, 0}, .root = {4, 0}},
    {.value = {0x11, 0, 0, 0}, .root = {4, 0}},
    {.value = {0xde0b6b3a7640000, 0, 0, 0}, .root = {0x3b9aca00, 0}},
    {.value = {0xb34b9f1000000000, 0xc097ce7bc90715, 0, 0},
     .root = {0xde0b6b3a7640000, 0}},
    {.value = {0, 0x31cdcf66f634e100, 0x3a4abc8955e946fe, 0x90e40fbeea1d},
     .root = {0xb34b9f1000000000, 0xc097ce7bc90715}},
    {.value = {0, 0xaa987b6e6fd2a000, 0x49ef0eb713f39ebe, 0xdd15fe86affad912},
     .root = {0x3ce3f43a577e553b, 0xede7409c18d38293}},
    {.value = {kMaxLimb, kMaxLimb, 0, 0}, .root = {kMaxLimb, 0}},
    {.value = {0, 0, 1, 0}, .root = {0, 1}},
    {.value = {1, 0, 1, 0}, .root = {0, 1}},
    {.value = {0, 0, 0, kTopBit},
     .root = {0x597d89b3754abe9f, 0xb504f333f9de6484}},
    {.value = {kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb},
     .root = {kMaxLimb, kMaxLimb}},
    {.value = {1, 0, kMaxLimb - 1, kMaxLimb}, .root = {kMaxLimb, kMaxLimb}},
    {.value = {0, 0, kMaxLimb - 1, kMaxLimb}, .root = {kMaxLimb - 1, kMaxLimb}},
    {.value = {0, 0, 0, 0x4000000000000000}, .root = {0, kTopBit}},
    {.value = {kMaxLimb, kMaxLimb, kMaxLimb, 0x3fffffffffffffff},
     .root = {kMaxLimb, kTopBit - 1}},
    {.value = {0, 0xeeeb4be2e32a2000, 0xec96957d0ca52f2, 0x2c37994e23322b6a},
     .root = {0x31ba5be94803fff0, 0x6a64c33195499e07}},
    {.value = {0x69a09cb03b63cd5f, 0x655f32b3afe16f01, 0x15743f591f4ee209,
               0xc0edbe718c346fa1},
     .root = {0xf625233f8e23aabe, 0xde3cefd2053e5c6d}},
    {.value = {0xa61b850051799876, 0xe3f6673cbf50f9a6, 0x1f64a68e7d57a798,
               0x48},
     .root = {0x266f23388343d791, 0x87e14c34a}},
    {.value = {0x985fa535af3a233d, 0xabce3acd197f957f, 0, 0},
     .root = {0xd1b82b0f6a9f50ae, 0}},
};

TEST(WideMathTest, IsqrtVectors)
{
  for (const SqrtVector &v : kSqrtVectors)
    EXPECT_EQ(limbs_isqrt(v.value), v.root) << hex(v.value);
}

// root is floor(sqrt(value)) exactly when root^2 <= value < (root + 1)^2.
void
expect_isqrt(const Limbs<4> &value)
{
  const Limbs<2> root = limbs_isqrt(value);
  const Ref square = ref_mul(Ref(root), Ref(root));
  EXPECT_FALSE(Ref(value) < square) << hex(value) << " root " << hex(root);
  // (root + 1)^2 == root^2 + 2 * root + 1.
  Ref next = square;
  next.add(Ref(root));
  next.add(Ref(root));
  next.add(Ref(from_u64<2>(1)));
  EXPECT_TRUE(Ref(value) < next) << hex(value) << " root " << hex(root);
}

TEST(WideMathTest, IsqrtPerfectSquaresAndNeighbours)
{
  std::mt19937_64 rng(7);
  std::vector<Limbs<2>> roots = boundary_values<2>();
  roots.push_back({0xffffffff, 0});
  roots.push_back({0x100000000, 0});
  roots.push_back({0, 0x100000000});
  roots.push_back({kMaxLimb - 1, kMaxLimb});
  // A low limb of all ones is where the division's quotient does not fit.
  roots.push_back({kMaxLimb, 1});
  roots.push_back({kMaxLimb, kTopBit});
  roots.push_back({kMaxLimb, kTopBit - 1});
  roots.push_back({kMaxLimb, 0x123456789abcdef0});
  roots.push_back({0, kMaxLimb});
  roots.push_back({1, kMaxLimb});
  for (size_t bits = 1; bits <= 128; ++bits) {
    roots.push_back(random_bits<2>(rng, bits));
    roots.push_back(random_edge_limbs<2>(rng));
  }
  const Ref one(from_u64<2>(1));
  for (const Limbs<2> &root : roots) {
    const Ref square = ref_mul(Ref(root), Ref(root));
    EXPECT_EQ(limbs_isqrt(square.limbs<4>()), root) << hex(root);

    // The largest value with this root is (root + 1)^2 - 1.
    Ref last = square;
    last.add(Ref(root));
    last.add(Ref(root));
    EXPECT_EQ(limbs_isqrt(last.limbs<4>()), root) << hex(root);

    Ref next_root(root);
    next_root.add(one);
    if (next_root.fits(2)) {
      last.add(one);
      EXPECT_EQ(limbs_isqrt(last.limbs<4>()), next_root.limbs<2>())
          << hex(root);
    }
    if (root != Limbs<2>{}) {
      Ref previous_root(root);
      previous_root.sub(one);
      Ref below = square;
      below.sub(one);
      EXPECT_EQ(limbs_isqrt(below.limbs<4>()), previous_root.limbs<2>())
          << hex(root);
    }
  }
}

TEST(WideMathTest, IsqrtEveryBitWidth)
{
  std::mt19937_64 rng(8);
  for (size_t bits = 0; bits <= 256; ++bits) {
    expect_isqrt(random_bits<4>(rng, bits));
    expect_isqrt(random_edge_limbs<4>(rng));
    if (::testing::Test::HasFailure())
      return;
  }
}

// The whole 256-bit routine against the one-bit-at-a-time root, on values of
// every length, on limbs at their edges, and around perfect squares, where
// the last correction is or is not taken.
TEST(WideMathTest, IsqrtMatchesBitByBit)
{
  std::mt19937_64 rng(10);
  const auto check = [](const Limbs<4> &value) {
    ASSERT_EQ(limbs_isqrt(value), bit_by_bit_isqrt(value)) << hex(value);
  };
  for (size_t bits = 0; bits <= 256; ++bits) {
    for (int i = 0; i < 20; ++i) {
      check(random_bits<4>(rng, bits));
      check(random_edge_limbs<4>(rng));
      if (::testing::Test::HasFailure())
        return;
    }
  }
  const Ref one(from_u64<4>(1));
  for (int i = 0; i < 4000; ++i) {
    // A root of any length whose limbs are random or at an edge.
    Limbs<2> root = i % 2 == 0 ? random_bits<2>(rng, rng() % 129)
                               : random_edge_limbs<2>(rng);
    if (i % 5 == 0)
      root[0] = kMaxLimb;
    const Ref square = ref_mul(Ref(root), Ref(root));
    check(square.limbs<4>());
    Ref last = square;
    last.add(Ref(root));
    last.add(Ref(root));
    check(last.limbs<4>());
    if (root != Limbs<2>{}) {
      Ref below = square;
      below.sub(one);
      check(below.limbs<4>());
    }
    if (::testing::Test::HasFailure())
      return;
  }
}

TEST(WideMathTest, ShlMatchesReference)
{
  std::mt19937_64 rng(11);
  for (size_t shift = 0; shift < 256; ++shift) {
    const Limbs<4> value = random_edge_limbs<4>(rng);
    Ref doubled(value);
    for (size_t i = 0; i < shift; ++i)
      doubled.twice();
    ASSERT_EQ(limbs_shl(value, shift), doubled.limbs<4>())
        << hex(value) << " << " << shift;
  }
}

// Whether root is floor(sqrt(value)), in the compiler's 128-bit arithmetic.
// (root + 1)^2 does not fit 128 bits for the largest root, so the upper bound
// is taken as value - root^2 <= 2 * root.
bool
is_floor_sqrt(uint128_t value, uint64_t root)
{
  const uint128_t square = uint128_t{root} * root;
  return square <= value && value - square <= 2 * uint128_t{root};
}

// The first steps of the root, at every value they can meet. The bits below
// are all zero, all one and random: all zero under a square is a square,
// which no step corrects, and all one under the value below a square has a
// root that is all ones below the top, where the quotients are at their
// largest.
TEST(WideMathTest, IsqrtNormalizedEveryShortPrefix)
{
  std::mt19937_64 rng(12);
  const auto check = [&rng](uint128_t prefix, uint32_t prefix_bits) {
    const uint32_t below = 128 - prefix_bits;
    const uint128_t ones = (uint128_t{1} << below) - 1;
    const uint128_t random = ((uint128_t{rng()} << 64) | rng()) & ones;
    for (const uint128_t low : {uint128_t{0}, ones, random}) {
      const uint128_t value = (prefix << below) | low;
      ASSERT_TRUE(is_floor_sqrt(value, isqrt_normalized_u128(value)))
          << hex(Limbs<2>{static_cast<uint64_t>(value),
                          static_cast<uint64_t>(value >> 64)});
    }
  };
  // Every value of the top 16 bits.
  for (uint32_t prefix = 0x4000; prefix <= 0xffff; ++prefix) {
    check(prefix, 16);
    if (::testing::Test::HasFailure())
      return;
  }
  // Every root of the top 32 bits: at its square, at the largest value that
  // still has it, and at the value below its square.
  for (uint64_t root = 0x8000; root <= 0xffff; ++root) {
    check(root * root, 32);
    check((root * root) + (2 * root), 32);
    if (root != 0x8000)
      check((root * root) - 1, 32);
    if (::testing::Test::HasFailure())
      return;
  }
}

TEST(WideMathTest, IsqrtNormalizedMatchesNativeArithmetic)
{
  std::mt19937_64 rng(13);
  const auto check = [](uint128_t value) {
    ASSERT_TRUE(is_floor_sqrt(value, isqrt_normalized_u128(value)))
        << hex(Limbs<2>{static_cast<uint64_t>(value),
                        static_cast<uint64_t>(value >> 64)});
  };
  for (int i = 0; i < 200000; ++i) {
    // 127 or 128 bits, random or with limbs at their edges.
    Limbs<2> limbs = i % 2 == 0 ? random_bits<2>(rng, 127 + (rng() % 2))
                                : random_edge_limbs<2>(rng);
    if ((limbs[1] >> 62) == 0)
      limbs[1] |= uint64_t{1} << (62 + (rng() % 2));
    check(limbs_to_u128(limbs));
    // Around the square of a root with its top bit set, which is the root of
    // such a value.
    const uint128_t root =
        (i % 3 == 0 ? random_edge_limbs<1>(rng)[0] : rng()) | kTopBit;
    check(root * root);
    check((root * root) + (2 * root));
    if (root != kTopBit)
      check((root * root) - 1);
    if (::testing::Test::HasFailure())
      return;
  }
}

} // namespace
} // namespace hogd

// NOLINTEND(bugprone-unchecked-optional-access)
