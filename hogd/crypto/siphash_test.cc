#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"

namespace hogd {

TEST(Siphash24, NullInput)
{
  ASSERT_TRUE(crypto_init());

  std::span<std::byte> null_input{};
  Siphash24Key key = siphash24_keygen();
  uint64_t mac = siphash24(null_input, key);
  EXPECT_EQ(mac, siphash24(null_input, key));

  auto key2 = siphash24_keygen();
  EXPECT_NE(mac, siphash24(null_input, key2));
}

TEST(Siphash24, SomeInput)
{
  ASSERT_TRUE(crypto_init());

  std::vector<uint8_t> bytes = {0, 1, 2, 3};
  std::vector<uint8_t> bytes2 = {3, 2, 1, 0};

  auto input = std::as_bytes(std::span(bytes));
  auto input2 = std::as_bytes(std::span(bytes2));

  // the same key should produce the same output for the same input value.
  Siphash24Key key = siphash24_keygen();
  EXPECT_EQ(siphash24(input, key), siphash24(input, key));
  EXPECT_NE(siphash24(input, key), siphash24(input2, key));

  // a different key should produce a different output.
  Siphash24Key key2 = siphash24_keygen();
  EXPECT_NE(siphash24(input, key), siphash24(input, key2));
}

} // namespace hogd
