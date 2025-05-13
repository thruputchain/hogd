#include <gtest/gtest.h>
#include <limits>

#include "hogd/base/types.h"
#include "hogd/storage_item/wide_integral.h"

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
  EXPECT_EQ(a.get_capped<uint64_t>(), 0);
  EXPECT_EQ(a.get_capped<int32_t>(), -50);

  EXPECT_TRUE(a.is_in_range<int64_t>());
  EXPECT_FALSE(a.is_in_range<uint64_t>());
  EXPECT_FALSE(a.is_in_range<uint8_t>());

  a -= 500;

  EXPECT_FALSE(a.is_in_range<uint8_t>());
  EXPECT_EQ(a.get_capped<uint8_t>(), 0);

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
  hogd::WideIntegral<uint8_t> a(100);
  a += 100;
  EXPECT_EQ(a.get_capped<uint8_t>(), 200);
  EXPECT_EQ(a.get_capped<int8_t>(), 127);
  a -= 250;
  a -= 250;
  EXPECT_EQ(a.get_capped<uint8_t>(), 0);
  EXPECT_EQ(a.get_capped<int8_t>(), -128);
}

TEST(WideIntegralTest, Assignment) {
  // Test assignment operator
  hogd::WideIntegral<uint8_t> a(100);
  a = 50;
  EXPECT_TRUE(a.is_in_range<uint8_t>());
  EXPECT_EQ(a.get_capped<uint8_t>(), 50);

  a = 255;
  EXPECT_TRUE(a.is_in_range<uint8_t>());
  EXPECT_EQ(a.get_capped<uint8_t>(), 255);
}

TEST(WideIntegralTest, EdgeCases) {
  // Test edge cases around type limits
  hogd::WideIntegral<int8_t> a(127); // max int8_t
  EXPECT_TRUE(a.is_in_range<int8_t>());
  a += 1;
  EXPECT_FALSE(a.is_in_range<int8_t>());
  EXPECT_EQ(a.get_capped<int8_t>(), 127);

  hogd::WideIntegral<int8_t> b(-128); // min int8_t
  EXPECT_TRUE(b.is_in_range<int8_t>());
  b -= 1;
  EXPECT_FALSE(b.is_in_range<int8_t>());
  EXPECT_EQ(b.get_capped<int8_t>(), -128);

  hogd::WideIntegral<uint8_t> c(255); // max uint8_t
  EXPECT_TRUE(c.is_in_range<uint8_t>());
  c += 1;
  EXPECT_FALSE(c.is_in_range<uint8_t>());
  EXPECT_EQ(c.get_capped<uint8_t>(), 255);
}

TEST(WideIntegralTest, LargeValues) {
  hogd::WideIntegral<uint128_t> a(std::numeric_limits<uint128_t>::max() - 5);
  EXPECT_TRUE(a.is_in_range<uint128_t>());
  EXPECT_EQ(a.get_capped<uint128_t>(), std::numeric_limits<uint128_t>::max() - 5);

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
} // namespace hogd
