#include <gtest/gtest.h>

#include <concepts>
#include <limits>
#include <memory>

#include "hogd/base/bits.h"
#include "hogd/base/types.h"

namespace hogd {

TEST(BitsTest, Alignment)
{
  // cases without an aligned val
  uint64_t val = 0x1234;
  EXPECT_EQ(align_up(val, 0x1000), 0x2000U);
  EXPECT_EQ(align_down(val, 0x1000), 0x1000U);
  EXPECT_EQ(align_up(val, 0x200), 0x1400U);
  EXPECT_EQ(align_down(val, 0x200), 0x1200U);

  // cases with an already aligned val
  val = 0x1000;
  EXPECT_EQ(align_up(val, 0x200), 0x1000U);
  EXPECT_EQ(align_down(val, 0x200), 0x1000U);
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
    EXPECT_EQ(T(s + delta - s), delta);
  };

  do_check.template operator()<uint8_t>(10);
  do_check.template operator()<uint16_t>(10);
  do_check.template operator()<uint32_t>(10);
  do_check.template operator()<uint64_t>(10);
}

TEST(BitsTest, TaggedPointer)
{
  static constexpr uint8_t kTag = 0x3B;
  std::unique_ptr tmp = std::make_unique<int>();

  // test constructors, setters, and getters
  TaggedPointer<int> tptr1(tmp.get(), kTag);
  TaggedPointer<int> tptr2(tmp.get());
  tptr2.set_tag(kTag);
  TaggedPointer<int> tptr3;
  tptr3 = tmp.get();
  tptr3.set_tag(kTag);
  EXPECT_EQ(tptr1, tmp.get());
  EXPECT_EQ(tptr2, tmp.get());
  EXPECT_EQ(tptr3, tmp.get());
  EXPECT_EQ(tptr1.get_tag(), kTag);
  EXPECT_EQ(tptr2.get_tag(), kTag);
  EXPECT_EQ(tptr3.get_tag(), kTag);

  // test dereferencing
  *tptr1 += 10;
  EXPECT_EQ(*tmp, 10);

  // test adjusting tag
  tptr1.set_tag(kTag + 1);
  EXPECT_EQ(tptr1.get(), tmp.get());
  EXPECT_EQ(tptr1.get_tag(), kTag + 1);
}

} // namespace hogd
