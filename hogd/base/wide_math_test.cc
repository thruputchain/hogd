#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/base/wide_math.h"

// Checked four independent ways: a table computed with Python, Ref below, the
// compiler's 128-bit arithmetic, and a root found one bit at a time.

namespace hogd {
namespace {

constexpr uint64_t kMaxLimb = UINT64_MAX;
constexpr uint64_t kTopBit = uint64_t{1} << 63;

// A deliberately naive unsigned integer: 32-bit words, one bit at a time. 1024
// bits hold any product a test forms.
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

// The compiler evaluates these, and it rejects undefined behaviour in a
// constant expression: an oversized shift, an index out of range or a read of
// an uninitialized limb on any path they take would not compile.
static_assert(limbs_mul<2>({kMaxLimb, kMaxLimb}, {kMaxLimb, kMaxLimb}) ==
              Limbs<4>{1, 0, kMaxLimb - 1, kMaxLimb});
static_assert(limbs_isqrt({kMaxLimb, kMaxLimb, kMaxLimb, kMaxLimb}) ==
              Limbs<2>{kMaxLimb, kMaxLimb});
static_assert(limbs_isqrt({0, 0, 1, 0}) == Limbs<2>{0, 1});
static_assert(limbs_isqrt({kMaxLimb, kMaxLimb, 0, 0}) == Limbs<2>{kMaxLimb, 0});
static_assert(limbs_isqrt({}) == Limbs<2>{});
static_assert(limbs_isqrt({3, 0, 0, 0}) == Limbs<2>{1, 0});
static_assert(limbs_shl<2>({1, 0}, 127) == Limbs<2>{0, kTopBit});
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

  const uint128_t value =
      (uint128_t{0x0123456789abcdef} << 64) | 0xfedcba9876543210;
  EXPECT_TRUE(limbs_to_u128({0xfedcba9876543210, 0x0123456789abcdef}) == value);
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
