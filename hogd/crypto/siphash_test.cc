#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"

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

  std::vector<uint8_t> bytes = {0, 1, 2, 3};
  std::vector<uint8_t> bytes2 = {3, 2, 1, 0};

  auto input = std::as_bytes(std::span(bytes));
  auto input2 = std::as_bytes(std::span(bytes2));

  auto key = siphash24_keygen();

  EXPECT_EQ(siphash24(input, key), siphash24(input, key));
  EXPECT_NE(siphash24(input, key), siphash24(input2, key));

  auto key2 = siphash24_keygen();

  EXPECT_NE(siphash24(input, key), siphash24(input, key2));
}

} // namespace hogd
