#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

#include "hogd/crypto/initialize.h"
#include "hogd/crypto/siphash24.h"

namespace hogd {

TEST(Siphash24, NullInput)
{
  ASSERT_TRUE(initialize_crypto());

  std::span<std::byte> null_input{};

  auto key = siphash24_keygen();

  auto res = siphash24(null_input, key);

  EXPECT_EQ(res, siphash24(null_input, key));

  auto key2 = siphash24_keygen();

  EXPECT_NE(res, siphash24(null_input, key2));
}

TEST(Siphash24, SomeInput)
{
  ASSERT_TRUE(initialize_crypto());

  std::vector<std::byte> bytes = {std::byte{0}, std::byte{1}, std::byte{2},
                                  std::byte{3}};
  std::vector<std::byte> bytes2 = {std::byte{3}, std::byte{2}, std::byte{1},
                                   std::byte{0}};

  std::span<std::byte> input(bytes);
  std::span<std::byte> input2(bytes2);

  auto key = siphash24_keygen();

  EXPECT_EQ(siphash24(input, key), siphash24(input, key));
  EXPECT_NE(siphash24(input, key), siphash24(input2, key));

  auto key2 = siphash24_keygen();

  EXPECT_NE(siphash24(input, key), siphash24(input, key2));
}

} // namespace hogd
