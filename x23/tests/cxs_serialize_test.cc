#include <array>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "x23/format.h"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"
#include "xdrtest.hh"

namespace x23 {

TEST(CxsSerialize, Int8RoundTrip)
{
  for (int8_t i = std::numeric_limits<int8_t>::min();
       i != std::numeric_limits<int8_t>::max(); ++i) {
    x23::Buf buf;
    ASSERT_TRUE(CxsSerializer<int8_t>::store(buf, i));
    auto view = x23::BufView(buf);
    if (i < kCxsInt81ByteMin || i > kCxsInt81ByteMax) {
      EXPECT_EQ(buf.size(), 2U);
    }
    else {
      EXPECT_EQ(buf.size(), 1U);
    }

    int8_t j;
    auto result = load_cxs_int<int8_t>(view);
    ASSERT_TRUE(result);
    j = *result;
    EXPECT_EQ(i, j);
  }
}

TEST(CxsSerialize, UInt8RoundTrip)
{
  for (uint8_t i = 0; i != std::numeric_limits<uint8_t>::max(); ++i) {
    x23::Buf buf;
    ASSERT_TRUE(CxsSerializer<uint8_t>::store(buf, i));
    auto view = x23::BufView(buf);
    if (i > kCxsUInt81ByteMax) {
      EXPECT_EQ(buf.size(), 2U);
    }
    else {
      EXPECT_EQ(buf.size(), 1U);
    }

    uint8_t j;
    auto result = load_cxs_uint<uint8_t>(view);
    ASSERT_TRUE(result);
    j = *result;
    EXPECT_EQ(i, j);
  }
}

namespace {
template<typename T>
void
check_roundtrip(T const &input)
{
  auto serialized = serialize_cxs(input);
  ASSERT_TRUE(serialized);
  x23::Buf &buf = *serialized;
  auto view = x23::BufView(buf);
  T output;
  ASSERT_TRUE(deserialize_cxs(&output, view));
  EXPECT_EQ(input, output);
}

template<typename T>
void
check_expect(T const &input, x23::Buf const &expected)
{
  auto serialized = serialize_cxs(input);
  ASSERT_TRUE(serialized);
  EXPECT_EQ(*serialized, expected);
}
} // namespace

TEST(CxsSerialize, BadDiscriminant)
{
  sunion_no_void value(static_cast<senum>(2));
  auto serialized = serialize_cxs(value);
  ASSERT_FALSE(serialized);
  EXPECT_EQ(serialized.error(), CxsErrorCode::kBadDiscriminant);
}

TEST(CxsSerialize, LargerIntRoundTrip)
{
  std::random_device rd;
  std::uniform_int_distribution<int64_t> dist(
      std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
  for (size_t count = 0; count < 10000; ++count) {
    int64_t i = dist(rd);
    check_roundtrip(i);
  }
}

// check uint edge cases
TEST(CxsSerialize, Uint32RoundTrip)
{
  check_roundtrip(uint32_t{0});
  check_roundtrip(uint32_t{255});
  check_roundtrip(uint32_t{256});
  check_roundtrip(uint32_t{65535});
  check_roundtrip(uint32_t{65536});
  check_roundtrip(std::numeric_limits<uint32_t>::max());
}

TEST(CxsSerialize, Uint64RoundTrip)
{
  check_roundtrip(uint64_t{0});
  check_roundtrip(uint64_t{255});
  check_roundtrip(uint64_t{256});
  check_roundtrip(uint64_t{65535});
  check_roundtrip(uint64_t{65536});
  check_roundtrip(uint64_t{1ULL << 32} - 1);
  check_roundtrip(uint64_t{1ULL << 32});
  check_roundtrip(std::numeric_limits<uint64_t>::max());
}

TEST(CxsSerialize, Int64RoundTrip)
{
  check_roundtrip(int64_t{0});
  check_roundtrip(int64_t{0} - 1);

  check_roundtrip(int64_t{std::numeric_limits<int8_t>::max()});
  check_roundtrip(int64_t{std::numeric_limits<int8_t>::min()});
  check_roundtrip(int64_t{std::numeric_limits<int8_t>::max()} + 1);
  check_roundtrip(int64_t{std::numeric_limits<int8_t>::min()} - 1);

  check_roundtrip(int64_t{std::numeric_limits<int16_t>::max()});
  check_roundtrip(int64_t{std::numeric_limits<int16_t>::min()});
  check_roundtrip(int64_t{std::numeric_limits<int16_t>::max()} + 1);
  check_roundtrip(int64_t{std::numeric_limits<int16_t>::min()} - 1);

  check_roundtrip(int64_t{std::numeric_limits<int32_t>::max()});
  check_roundtrip(int64_t{std::numeric_limits<int32_t>::min()});
  check_roundtrip(int64_t{std::numeric_limits<int32_t>::max()} + 1);
  check_roundtrip(int64_t{std::numeric_limits<int32_t>::min()} - 1);

  check_roundtrip(std::numeric_limits<int64_t>::min());
  check_roundtrip(std::numeric_limits<int64_t>::max());
}

TEST(CxsSerialize, StructRoundTrip)
{
  using test_struct = NonnegativeInt64SetAddArgs;
  test_struct value;
  value.set_value = 123;
  value.add_value = 456;
  check_roundtrip(value);
}

TEST(CxsSerialize, UnionRoundTrip)
{
  using test_union = sunion_no_void;
  test_union value(test_union::SE_POSITIVE);
  value.pos() = 123;
  check_roundtrip(value);
}

TEST(CxsSerialize, ConstexprUnion)
{
  constexpr auto value = [] {
    sunion_no_void out(senum::SE_POSITIVE);
    out.pos() = 123;
    return out;
  }();
  constexpr auto copy = value;
  constexpr auto assigned = [copy] {
    sunion_no_void out(senum::SE_NEGATIVE);
    out = copy;
    return out;
  }();
  static_assert(copy == value);
  static_assert((copy <=> value) == 0);
  static_assert(assigned.pos() == 123);

  constexpr auto serialized = serialize_cxs_array<cxs_size(assigned)>(assigned);
  static_assert(serialized == std::array<uint8_t, 3>{1, 0x3f, 123});
}

TEST(CxsSerialize, VecRoundTrip)
{
  using test_vec = Bytestring;
  test_vec value;
  value.append_range("hello");
  check_roundtrip(value);
}

TEST(CxsSerialize, StorageDeltaRoundTrip)
{
  StorageDelta value(StorageDelta::kBytestringSet);
  x23::Buf buf;
  buf.append_range("world");
  value.bstring() = buf;
  check_roundtrip(value);
}

TEST(CxsSerialize, RecursiveRoundTrip)
{
  using test_struct = test_recursive;
  test_struct value;
  value.elem = "hello";
  value.next.emplace();
  value.next->elem = "world";
  value.next->next.emplace();
  value.next->next->elem = "!";
  value.next->next->next.emplace();
  value.next->next->next->elem = "?";
  value.next->next->next->next.emplace();
  value.next->next->next->next->elem = ".";
  check_roundtrip(value);
}

TEST(CxsSerialize, OpaqueVecRoundTrip)
{
  using test_array = x23::Vec<uint8_t, 1024>;
  test_array value;
  value.resize_uninitialized(256);
  for (size_t i = 0; i < 256; ++i) {
    value[i] = i;
  }
  check_roundtrip(value);
}

TEST(CxsSerialize, ArrayRoundTrip)
{
  using test_array = std::array<fix_12, 100>;
  test_array value;
  for (size_t i = 0; i < value.size(); ++i) {
    int32_t x = i;
    double d = i * 2;
    value[i] = {.i = x, .d = d};
  }
  check_roundtrip(value);
}

TEST(CxsSerialize, FloatRoundTrip)
{
  check_roundtrip(float{0.0});
  check_roundtrip(float{1.0});
  check_roundtrip(float{-1.0});
  check_roundtrip(std::numeric_limits<float>::max());
  check_roundtrip(std::numeric_limits<float>::min());
}

TEST(CxsSerialize, CanonicalCheck)
{
  x23::Buf buf;
  buf.push_back(kCxsHeader1Byte);
  buf.push_back(10);
  x23::BufView view(buf);
  auto result = load_cxs_uint<uint8_t>(view);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CxsErrorCode::kNonCanonical);
}

TEST(CxsSerialize, Int8Encoding)
{
  check_expect(int32_t{0}, {0x00});
  check_expect(int32_t{1}, {0x01});
  check_expect(int32_t{-1}, {0x3e});
  check_expect(int32_t{-31}, {0x20});
  check_expect(int32_t{31}, {0x1f});
}

TEST(Vec, SelfMoveAssignPreservesData)
{
  x23::Vec<uint32_t, 16> v;
  v.push_back(1);
  v.push_back(2);
  v.push_back(3);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
  v = std::move(v);
#pragma GCC diagnostic pop

  ASSERT_EQ(v.size(), 3UL);
  EXPECT_EQ(v[0], 1U);
  EXPECT_EQ(v[1], 2U);
  EXPECT_EQ(v[2], 3U);
}

namespace {

box_arms_plain
plain_arm(const box_arms &u)
{
  using enum box_arm_type;
  box_arms_plain p(u.t());
  switch (u.t()) {
  case kBoxArmSmall:
    p.small() = u.small();
    break;
  case kBoxArmBig:
    p.big() = *u.big();
    break;
  case kBoxArmStruct:
    p.st() = *u.st();
    break;
  case kBoxArmBytes:
    p.bytes() = *u.bytes();
    break;
  case kBoxArmInts:
    p.ints() = *u.ints();
    break;
  }
  return p;
}

} // namespace

// A boxed union arm or field encodes exactly as its unboxed twin, including an
// arm that was selected but never written.
TEST(CxsSerialize, BoxedLeavesMatchPlain)
{
  using enum box_arm_type;
  std::vector<box_arms> arms;
  for (auto t :
       {kBoxArmSmall, kBoxArmBig, kBoxArmStruct, kBoxArmBytes, kBoxArmInts})
    arms.emplace_back(t);
  box_arms big(kBoxArmBig);
  big.big()->fill(0x5a);
  arms.push_back(big);
  box_arms st(kBoxArmStruct);
  *st.st() = fix_12{.i = -7, .d = 2.5};
  arms.push_back(st);
  box_arms bytes(kBoxArmBytes);
  bytes.bytes() = box_bytes{1, 2, 3};
  arms.push_back(bytes);
  box_arms ints(kBoxArmInts);
  ints.ints()->push_back(-1);
  ints.ints()->push_back(300);
  arms.push_back(ints);

  for (const box_arms &u : arms) {
    box_arms_plain p = plain_arm(u);
    auto boxed = serialize_cxs(u);
    auto plain = serialize_cxs(p);
    ASSERT_TRUE(boxed.has_value());
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(*boxed, *plain);
    EXPECT_EQ(cxs_size(u), boxed->size());

    box_arms back;
    ASSERT_TRUE(deserialize_cxs(&back, *boxed));
    EXPECT_EQ(back, u);
  }

  box_leaves leaves{};
  box_leaves_plain leaves_plain{};
  *leaves.n = 0x0102030405060708;
  leaves_plain.n = 0x0102030405060708;
  leaves.bytes = box_bytes{9, 8};
  leaves_plain.bytes = box_bytes{9, 8};
  leaves.arm = big;
  leaves_plain.arm = plain_arm(big);
  auto boxed = serialize_cxs(leaves);
  ASSERT_TRUE(boxed.has_value());
  EXPECT_EQ(*boxed, *serialize_cxs(leaves_plain));
  EXPECT_EQ(cxs_size(leaves), boxed->size());
  box_leaves back;
  ASSERT_TRUE(deserialize_cxs(&back, *boxed));
  EXPECT_EQ(back, leaves);

  EXPECT_EQ(show(leaves), show(leaves_plain));
}

// A boxed union field encodes exactly as its unboxed twin.
TEST(CxsSerialize, BoxedUnionFieldMatchesPlain)
{
  using enum box_arm_type;
  box_union_field f{};
  box_union_field_plain p{};
  EXPECT_EQ(*serialize_cxs(f), *serialize_cxs(p));

  f.u->t(kBoxArmStruct);
  f.u->st() = fix_12{.i = -7, .d = 2.5};
  f.tail = 3;
  p.u.t(kBoxArmStruct);
  p.u.st() = fix_12{.i = -7, .d = 2.5};
  p.tail = 3;

  auto boxed = serialize_cxs(f);
  ASSERT_TRUE(boxed.has_value());
  EXPECT_EQ(*boxed, *serialize_cxs(p));
  EXPECT_EQ(cxs_size(f), boxed->size());
  box_union_field back;
  ASSERT_TRUE(deserialize_cxs(&back, *boxed));
  EXPECT_EQ(back, f);
  EXPECT_EQ(show(f), show(p));
}

} // namespace x23
