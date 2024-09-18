#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <span>
#include <vector>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"

namespace hogd {

TEST(SHA256, NullInput)
{
  std::span<std::byte> null_input{};
  SHA256Hash hash = sha256(null_input);
  EXPECT_NE(hash, SHA256Hash{});
}

TEST(SHA256, SomeInput)
{
  std::vector<uint8_t> bytes = {0, 1, 2, 3};
  std::vector<uint8_t> bytes2 = {3, 2, 1, 0};
  EXPECT_NE(sha256(std::as_bytes(std::span(bytes))),
            sha256(std::as_bytes(std::span(bytes2))));
}

TEST(Siphash24, NullInput)
{
  std::span<std::byte> null_input{};
  Siphash24Key key = siphash24_keygen();
  uint64_t mac = siphash24(null_input, key);
  EXPECT_EQ(mac, siphash24(null_input, key));

  auto key2 = siphash24_keygen();
  EXPECT_NE(mac, siphash24(null_input, key2));
}

TEST(Siphash24, SomeInput)
{
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

int
main(int argc, char *argv[])
{
  using hogd::GStatus;
  GStatus<void> ret = hogd::crypto_init();
  if (!ret)
    return EXIT_FAILURE;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
