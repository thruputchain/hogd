#include <gtest/gtest.h>

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

} // namespace hogd
