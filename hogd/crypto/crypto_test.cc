#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"

namespace hogd {

TEST(SHA256, BasicUsage)
{
  std::vector<uint8_t> a = {0, 1, 2, 3};
  std::vector<uint8_t> a_same = {0, 1, 2, 3};
  std::vector<uint8_t> b = {3, 2, 1, 0};

  EXPECT_EQ(sha256(a), sha256(a_same));
  EXPECT_NE(sha256(a), sha256(b));
}

TEST(Blake3, BasicUsage)
{
  std::vector<uint8_t> a = {0, 1, 2, 3};
  std::vector<uint8_t> a_same = {0, 1, 2, 3};
  std::vector<uint8_t> b = {3, 2, 1, 0};

  EXPECT_EQ(blake3(a), blake3(a_same));
  EXPECT_NE(blake3(a), blake3(b));
}

TEST(CRC32C, BasicUsage)
{
  std::vector<uint8_t> a = {0, 1, 2, 3};
  std::vector<uint8_t> a_same = {0, 1, 2, 3};
  std::vector<uint8_t> b = {3, 2, 1, 0};

  // direct calcuation
  EXPECT_EQ(crc32c(a), crc32c(a_same));
  EXPECT_NE(crc32c(a), crc32c(b));

  // appended calculation
  uint32_t crc = crc32c(a);
  crc = crc32c_append(crc, b);
  a.insert(a.end(), b.begin(), b.end());
  EXPECT_EQ(crc, crc32c(a));
}

TEST(Siphash24, BasicUsage)
{
  std::vector<uint8_t> a = {0, 1, 2, 3};
  std::vector<uint8_t> a_same = {0, 1, 2, 3};
  std::vector<uint8_t> b = {3, 2, 1, 0};

  // the same key should produce different outputs for different inputs.
  SipHash24 hasher;
  EXPECT_EQ(hasher(a), hasher(a_same));
  EXPECT_NE(hasher(a), hasher(b));

  // different keys should produce different outputs for the same input.
  SipHash24 hasher2;
  EXPECT_NE(hasher.get_key(), hasher2.get_key());
  EXPECT_NE(hasher(a), hasher2(a));
  EXPECT_NE(hasher(b), hasher2(b));
}

TEST(Ed25519, BasicUsage)
{
  auto [pk, sk] = ed25519_keygen();
  EXPECT_EQ(pk.size(), 32);
  EXPECT_EQ(sk.size(), 64);

  std::string_view msg = "test message";
  std::string_view msg_wrong = "wrong message";
  auto sig = ed25519_sign(sk, msg);
  EXPECT_EQ(ed25519_verify(pk, sig, msg), true);
  EXPECT_EQ(ed25519_verify(pk, sig, msg_wrong), false);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
