#include <concepts>
#include <limits>

#include "hogd/base/bits.h"
#include "hogd/base/types.h"
#include <gtest/gtest.h>

namespace hogd {

TEST(BitsTest, Alignment)
{
  // cases without an aligned val
  uint64_t val = 0x1234;
  EXPECT_EQ(align_up(val, 0x1000), 0x2000);
  EXPECT_EQ(align_down(val, 0x1000), 0x1000);
  EXPECT_EQ(align_up(val, 0x200), 0x1400);
  EXPECT_EQ(align_down(val, 0x200), 0x1200);

  // cases with an already aligned val
  val = 0x1000;
  EXPECT_EQ(align_up(val, 0x200), 0x1000);
  EXPECT_EQ(align_down(val, 0x200), 0x1000);
}

TEST(BitsTest, DivideUp)
{
  EXPECT_EQ(divide_up(10, 8), 2);
  EXPECT_EQ(divide_up(16, 8), 2);
  EXPECT_EQ(divide_up(17, 8), 3);
}

TEST(BitsTest, WrappingSequence)
{
  auto do_check = []<std::unsigned_integral T>(T delta) {
    WrappingSequence<T> s(std::numeric_limits<T>::max());
    EXPECT_GT(s + delta, s);
    EXPECT_GE(s + delta, s);
    EXPECT_LT(s, s + delta);
    EXPECT_LE(s, s + delta);
    EXPECT_GE(s, s);
    EXPECT_LE(s, s);
    EXPECT_EQ(s + delta, s + delta);
    EXPECT_EQ(s + delta - s, delta);
  };

  do_check.template operator()<uint8_t>(10);
  do_check.template operator()<uint16_t>(10);
  do_check.template operator()<uint32_t>(10);
  do_check.template operator()<uint64_t>(10);
}

} // namespace hogd
