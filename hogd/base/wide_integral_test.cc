#include <gtest/gtest.h>
#include <limits>

#include "hogd/base/types.h"
#include "hogd/base/wide_integral.h"

namespace hogd {
TEST(WideIntegralTest, AddSub)
{
  hogd::WideIntegral<int64_t> a(100);
  a += 100;
  EXPECT_EQ(a.get_capped<int64_t>(), 200);

  EXPECT_TRUE(a.is_in_range<int64_t>());
  EXPECT_TRUE(a.is_in_range<uint64_t>());
  EXPECT_TRUE(a.is_in_range<uint8_t>());
  EXPECT_FALSE(a.is_in_range<int8_t>());
  EXPECT_EQ(a.get_capped<int8_t>(), 127);

  a -= 100;
  EXPECT_EQ(a.get_capped<int64_t>(), 100);

  a -= 150;
  EXPECT_EQ(a.get_capped<int64_t>(), -50);
  EXPECT_EQ(a.get_capped<uint64_t>(), 0U);
  EXPECT_EQ(a.get_capped<int32_t>(), -50);

  EXPECT_TRUE(a.is_in_range<int64_t>());
  EXPECT_FALSE(a.is_in_range<uint64_t>());
  EXPECT_FALSE(a.is_in_range<uint8_t>());

  a -= 500;

  EXPECT_FALSE(a.is_in_range<uint8_t>());
  EXPECT_EQ(a.get_capped<uint8_t>(), 0U);

  EXPECT_FALSE(a.is_in_range<int8_t>());
  EXPECT_EQ(a.get_capped<int8_t>(), -128);

  EXPECT_TRUE(a.is_in_range<int16_t>());
  EXPECT_EQ(a.get_capped<int16_t>(), -550);

  a += 550;

  // check boundary conditions
  a -= 128;
  EXPECT_TRUE(a.is_in_range<int8_t>());
  EXPECT_EQ(a.get_capped<int8_t>(), -128);
  a -= 1;
  EXPECT_FALSE(a.is_in_range<int8_t>());
  EXPECT_EQ(a.get_capped<int8_t>(), -128);
}

TEST(WideIntegralTest, Unsigned)
{
  hogd::WideIntegral<uint8_t> a(uint8_t{100});
  a += uint8_t{100};
  EXPECT_EQ(a.get_capped<uint8_t>(), 200U);
  EXPECT_EQ(a.get_capped<int8_t>(), 127);
  a -= uint8_t{250};
  a -= uint8_t{250};
  EXPECT_EQ(a.get_capped<uint8_t>(), 0U);
  EXPECT_EQ(a.get_capped<int8_t>(), -128);
}

TEST(WideIntegralTest, Assignment)
{
  // Test assignment operator
  hogd::WideIntegral<uint8_t> a(uint8_t{100});
  a = int8_t{50};
  EXPECT_TRUE(a.is_in_range<uint8_t>());
  EXPECT_EQ(a.get_capped<uint8_t>(), 50U);

  a = uint8_t{255};
  EXPECT_TRUE(a.is_in_range<uint8_t>());
  EXPECT_EQ(a.get_capped<uint8_t>(), 255U);
}

TEST(WideIntegralTest, EdgeCases)
{
  // Test edge cases around type limits
  hogd::WideIntegral<int8_t> a(uint8_t{127}); // max int8_t
  EXPECT_TRUE(a.is_in_range<int8_t>());
  a += int8_t{1};
  EXPECT_FALSE(a.is_in_range<int8_t>());
  EXPECT_EQ(a.get_capped<int8_t>(), 127);

  hogd::WideIntegral<int8_t> b(int8_t{-128}); // min int8_t
  EXPECT_TRUE(b.is_in_range<int8_t>());
  b -= uint8_t{1};
  EXPECT_FALSE(b.is_in_range<int8_t>());
  EXPECT_EQ(b.get_capped<int8_t>(), -128);

  hogd::WideIntegral<uint8_t> c(uint8_t{255}); // max uint8_t
  EXPECT_TRUE(c.is_in_range<uint8_t>());
  c += uint8_t{1};
  EXPECT_FALSE(c.is_in_range<uint8_t>());
  EXPECT_EQ(c.get_capped<uint8_t>(), 255U);
}

TEST(WideIntegralTest, LargeValues)
{
  hogd::WideIntegral<uint128_t> a(std::numeric_limits<uint128_t>::max() - 5);
  EXPECT_TRUE(a.is_in_range<uint128_t>());
  EXPECT_EQ(a.get_capped<uint128_t>(),
            std::numeric_limits<uint128_t>::max() - 5);

  a += 5;
  EXPECT_TRUE(a.is_in_range<uint128_t>());
  EXPECT_EQ(a.get_capped<uint128_t>(), std::numeric_limits<uint128_t>::max());

  a += 1;
  EXPECT_FALSE(a.is_in_range<uint128_t>());
  EXPECT_EQ(a.get_capped<uint128_t>(), std::numeric_limits<uint128_t>::max());

  a -= 1;
  EXPECT_TRUE(a.is_in_range<uint128_t>());
  EXPECT_EQ(a.get_capped<uint128_t>(), std::numeric_limits<uint128_t>::max());
}

TEST(WideIntegralTest, LargeTypeConversions)
{
  hogd::WideIntegral<int128_t> a(uint8_t{100});

  a += 1'000'000;

  EXPECT_EQ(a.get_capped<uint32_t>(), 1'000'100);
  a += std::numeric_limits<uint64_t>::max();
  EXPECT_EQ(a.get_capped<uint64_t>(), std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(a.get_capped<uint128_t>(),
            uint128_t{std::numeric_limits<uint64_t>::max()} + 1'000'100);

  a -= std::numeric_limits<int64_t>::max();

  EXPECT_EQ(a.get_capped<uint128_t>(),
            uint128_t{std::numeric_limits<int64_t>::max()} + 1'000'101);
}
} // namespace hogd
