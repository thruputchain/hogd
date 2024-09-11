#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

#include "hogd/crypto/siphash24.h"

namespace hogd {

TEST(Siphash24, NullInput)
{
  std::span<std::byte, std::dynamic_extent> null_input{};

  auto key = siphash24_keygen();

  ASSERT_TRUE(key);

  auto res = siphash24(null_input, key.value());

  EXPECT_TRUE(res);

  EXPECT_EQ(res.value(), siphash24(null_input, key.value()).value());

  auto key2 = siphash24_keygen();

  EXPECT_NE(res.value(), siphash24(null_input, key2.value()).value());
}

TEST(Siphash24, SomeInput)
{
  std::vector<std::byte> bytes = {std::byte{0}, std::byte{1}, std::byte{2},
                                  std::byte{3}};
  std::vector<std::byte> bytes2 = {std::byte{3}, std::byte{2}, std::byte{1},
                                   std::byte{0}};

  std::span<std::byte, std::dynamic_extent> input(bytes.begin(), bytes.end());
  std::span<std::byte, std::dynamic_extent> input2(bytes2.begin(),
                                                   bytes2.end());

  auto key = siphash24_keygen();

  ASSERT_TRUE(key);

  auto res = siphash24(input, key.value());

  EXPECT_TRUE(res);

  EXPECT_EQ(res.value(), siphash24(input, key.value()).value());
  EXPECT_NE(res.value(), siphash24(input2, key.value()).value());

  auto key2 = siphash24_keygen();

  EXPECT_NE(res.value(), siphash24(input, key2.value()).value());
}

} // namespace hogd
