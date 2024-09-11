#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

#include "hogd/crypto/crypto.h"

namespace hogd {

TEST(SHA256, NullInput)
{
  ASSERT_TRUE(initialize_crypto());

  std::span<std::byte> null_input{};

  auto res = sha256(null_input);

  EXPECT_NE(res, SHA256Hash{});
}

TEST(SHA256, SomeInput)
{
  ASSERT_TRUE(initialize_crypto());

  std::vector<std::byte> bytes = {std::byte{0}, std::byte{1}, std::byte{2},
                                  std::byte{3}};
  std::vector<std::byte> bytes2 = {std::byte{3}, std::byte{2}, std::byte{1},
                                   std::byte{0}};

  std::span<std::byte> input(bytes);
  std::span<std::byte> input2(bytes2);

  EXPECT_NE(sha256(input), sha256(input2));
}

} // namespace hogd
