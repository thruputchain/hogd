#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

#include "hogd/crypto/sha256.h"

namespace hogd {

TEST(SHA256, NullInput)
{
  std::span<std::byte, std::dynamic_extent> null_input{};

  auto res = sha256(null_input);

  EXPECT_TRUE(res);
}

TEST(SHA256, SomeInput)
{
  std::vector<std::byte> bytes = {std::byte{0}, std::byte{1}, std::byte{2},
                                  std::byte{3}};
  std::vector<std::byte> bytes2 = {std::byte{3}, std::byte{2}, std::byte{1},
                                   std::byte{0}};

  std::span<std::byte, std::dynamic_extent> input(bytes.begin(), bytes.end());
  std::span<std::byte, std::dynamic_extent> input2(bytes2.begin(),
                                                   bytes2.end());

  EXPECT_NE(sha256(input).value(), sha256(input2).value());
}

} // namespace hogd
