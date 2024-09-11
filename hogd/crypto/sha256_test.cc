#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

#include "hogd/base/types.h"
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

  std::vector<uint8_t> bytes = {0, 1, 2, 3};
  std::vector<uint8_t> bytes2 = {3, 2, 1, 0};

  EXPECT_NE(sha256(std::as_bytes(std::span(bytes))),
            sha256(std::as_bytes(std::span(bytes2))));
}

} // namespace hogd
