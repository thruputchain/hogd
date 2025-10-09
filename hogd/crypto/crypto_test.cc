#include <gtest/gtest.h>

#include <span>
#include <string_view>
#include <vector>

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
  auto sig = ed25519_sign(sk, std::as_bytes(std::span(msg)));
  EXPECT_EQ(ed25519_verify(pk, sig, msg), true);
  EXPECT_EQ(ed25519_verify(pk, sig, msg_wrong), false);
}

<<<<<<< HEAD
namespace {

bool check_sig(Ed25519PublicKey const& pk, Ed25519Signature const& sk, SHA256Hash const& nonce)
{
	return ed25519_verify(pk, sk, nonce);
}

}

TEST(Ed25519, Bug)
{
	auto [pk, sk] = ed25519_keygen();
	uint64_t v = 0;
	auto msg = sha256(v);
	auto sig = ed25519_sign(sk, msg);
	EXPECT_TRUE(check_sig(pk, sig, msg));
=======
// Catch an FFI UAF bug.
TEST(Ed25519, FFIBug)
{
  auto [pk, sk] = ed25519_keygen();
  uint64_t v = 0;
  auto msg = sha256(v);
  auto sig = ed25519_sign(sk, msg);
  EXPECT_TRUE(ed25519_verify(pk, sig, msg));
>>>>>>> main
}

TEST(Ed25519, BatchUsage)
{
  auto [pk1, sk1] = ed25519_keygen();
  auto [pk2, sk2] = ed25519_keygen();
  auto [pk3, sk3] = ed25519_keygen();

  std::string_view msg1 = "test message";
  std::string_view msg2 = "another message";
  std::string_view msg3 = "yet another message";

  auto sig1 = ed25519_sign(sk1, msg1);
  auto sig2 = ed25519_sign(sk2, msg2);
  auto sig3 = ed25519_sign(sk3, msg3);

  EXPECT_EQ(ed25519_verify_batch({pk1, pk2, pk3}, {sig1, sig2, sig3},
                                 {std::as_bytes(std::span(msg1)),
                                  std::as_bytes(std::span(msg2)),
                                  std::as_bytes(std::span(msg3))}),
            true);
}

TEST(Ed25519, RandomKeygen)
{
  auto [pk, sk] = ed25519_keygen();
  auto [pk2, sk2] = ed25519_keygen();
  EXPECT_NE(pk, pk2);
  EXPECT_NE(sk, sk2);
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
