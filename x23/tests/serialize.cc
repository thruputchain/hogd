#include <array>
#include <cstdint>
#include <tuple>
#include <variant>

#include <gtest/gtest.h>

#include "tests/xdrtest.hh"
#include "x23/serialize.h"

using namespace x23;

TEST(Serialize, SizeBoundsSaturate)
{
  using detail::SizeBounds;
  constexpr auto limit = detail::kNoBound;
  EXPECT_EQ((SizeBounds{4} + 8), (SizeBounds{12}));
  EXPECT_EQ((SizeBounds{4, limit} + SizeBounds{8}), (SizeBounds{12, limit}));
  EXPECT_EQ((SizeBounds{4, limit} + 1), (SizeBounds{5, limit}));
  EXPECT_EQ((SizeBounds{4, 8} * 3), (SizeBounds{12, 24}));
  EXPECT_EQ((SizeBounds{0, limit} * 2), (SizeBounds{0, limit}));
  EXPECT_EQ((SizeBounds{0} * 2), (SizeBounds{0}));
  EXPECT_EQ(SizeBounds(4, 8).zero_to(3), (SizeBounds{0, 24}));
  EXPECT_EQ(SizeBounds(0, limit).zero_to(0), (SizeBounds{0}));
}

TEST(Serialize, EmptyAndByteViews)
{
  EXPECT_EQ(xdr_size(std::monostate{}), 0UZ);
  EXPECT_TRUE(serialize(std::monostate{}).empty());
  for (size_t size = 0; size <= 8; ++size) {
    Buf bytes(size, 0xa5);
    auto encoded = serialize(BufView(bytes));
    EXPECT_EQ(encoded, serialize(bytes));
    EXPECT_EQ(encoded.size(), (size + 7) & ~3UZ);
    BufView view;
    deserialize(&view, BufView(encoded));
    EXPECT_EQ(view.size(), size);
    EXPECT_TRUE(std::ranges::equal(view, bytes));
    if (size) {
      EXPECT_EQ(view.data(), encoded.data() + 4);
    }
  }
}

TEST(Serialize, VariableSizeSpansAndProjections)
{
  std::array<Str, 2> strings{"a", "bcdef"};
  span<Str> dynamic(strings);
  span<Str, 2> fixed(strings);
  EXPECT_EQ(serialize(dynamic), serialize(Vec<Str>(std::from_range, strings)));
  EXPECT_EQ(serialize(fixed), serialize(strings));
  EXPECT_EQ(xdr_size(dynamic), 24UZ);
  EXPECT_EQ(xdr_size(fixed), 20UZ);
  struct Record {
    Str text;
    int ignored;
  };
  std::array<Record, 2> records{{{"a", 10}, {"bcdef", 20}}};
  ProjectedView<Record, &Record::text> projected(records);
  ProjectedView<Record, &Record::text, 2> projected_fixed(records);
  EXPECT_EQ(serialize(projected), serialize(dynamic));
  EXPECT_EQ(serialize(projected_fixed), serialize(fixed));
  EXPECT_EQ(xdr_size(projected), 24UZ);
  EXPECT_EQ(xdr_size(projected_fixed), 20UZ);
}

TEST(Serialize, InvalidUnionIsRejected)
{
  uunion value;
  union_set_discriminant(value, 99U);
  ASSERT_FALSE(union_valid(value));
  EXPECT_THROW(serialize(value), XdrBadUnionAccess);
}

TEST(Deserialize, InvalidBooleanAndTrailingData)
{
  bool value = false;
  for (uint32_t invalid : {2U, 0xffffffffU}) {
    auto bytes = serialize(invalid);
    EXPECT_THROW(deserialize(&value, BufView(bytes)), XdrBadInput);
  }
  auto bytes = serialize(uint32_t{1}, uint32_t{2});
  uint32_t out = 0;
  EXPECT_THROW(deserialize(&out, BufView(bytes)), XdrBadInput);
  EXPECT_EQ(deserialize_from(&out, BufView(bytes)), 4UZ);
  EXPECT_EQ(out, 1U);
}

TEST(Deserialize, TruncatedFixedAndVariableArrays)
{
  std::array<uint64_t, 2> integers{1, 2}, out{};
  auto bytes = serialize(integers);
  for (size_t size = 0; size < bytes.size(); size += 4)
    EXPECT_THROW(deserialize(&out, BufView(bytes.data(), size)), XdrBadInput);
  deserialize(&out, BufView(bytes));
  EXPECT_EQ(out, integers);

  std::array<Str, 2> strings{"a", "bcdef"}, result;
  bytes = serialize(strings);
  for (size_t size = 0; size < bytes.size(); size += 4)
    EXPECT_THROW(deserialize(&result, BufView(bytes.data(), size)),
                 XdrBadInput);
  deserialize(&result, BufView(bytes));
  EXPECT_EQ(result, strings);
}

TEST(Deserialize, ReadableTailStillRejectsLogicalTruncation)
{
  auto bytes = serialize(std::array<uint64_t, 2>{1, 2});
  std::array<uint64_t, 2> out{};
  BufViewEnd truncated(BufView(bytes.data(), 8), bytes.data() + bytes.size());
  EXPECT_EQ(truncated.bufend(), bytes.data() + bytes.size());
  EXPECT_THROW(deserialize(&out, truncated), XdrBadInput);
  // The optimized decoder may use readable tail space, but must reject it
  // when checking the logical input length at the end.
  EXPECT_EQ(out[1], 2U);
}

TEST(DeserializeDeathTest, CheckDetectsDecoderOverrunWithoutReadingMemory)
{
  std::array<uint8_t, 8> storage{};
  detail::Deserializer<false> decoder(storage.data(), storage.data() + 4,
                                      storage.data() + 4,
                                      deserialization_recursion_limit);
  decoder.p = storage.data() + 8;
  EXPECT_DEATH(decoder.check(), "buffer overrun");
}
