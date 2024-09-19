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

TEST(LtHash, Commutes)
{
  std::vector<uint8_t> bytes = {0, 1, 2, 3};
  std::vector<uint8_t> bytes2 = {3, 2, 1, 0};
  std::vector<uint8_t> bytes3 = {3, 2, 0, 1};

  auto input = std::as_bytes(std::span(bytes));
  auto input2 = std::as_bytes(std::span(bytes2));
  auto input3 = std::as_bytes(std::span(bytes3));

  const LtHash16 zero{}, a(input), b(input2), c(input3);

  LtHash16 x = a;
  x += zero;

  EXPECT_NE(zero, a);
  EXPECT_NE(a, b);
  EXPECT_NE(b, c);

  EXPECT_EQ(x, a);

  x += b;
  EXPECT_NE(x, a);

  LtHash16 xp = b;
  xp += a;
  EXPECT_EQ(x, xp);

  xp -= c;

  LtHash16 xpp = a;
  xpp -= c;
  xpp += b;

  EXPECT_EQ(xpp, xp);

  EXPECT_NE(xpp, a);
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
