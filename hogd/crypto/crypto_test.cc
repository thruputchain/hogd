#include <array>
#include <cstddef>
#include <gtest/gtest.h>

#include <span>
#include <string_view>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/lthash16.h"
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

TEST(SHA256, IncrementalMatchesOneShot)
{
  std::string_view msg = "incremental sha256 coverage";
  auto msg_bytes = std::as_bytes(std::span(msg));

  SHA256Hasher empty_hasher;
  EXPECT_EQ(empty_hasher.finalize(), sha256(std::span<const std::byte>{}));

  SHA256Hasher single_chunk_hasher;
  single_chunk_hasher.update(msg_bytes);
  EXPECT_EQ(single_chunk_hasher.finalize(), sha256(msg_bytes));

  SHA256Hasher multi_chunk_hasher;
  multi_chunk_hasher.update(msg_bytes.subspan(0, 5));
  multi_chunk_hasher.update(msg_bytes.subspan(5, 7));
  multi_chunk_hasher.update(msg_bytes.subspan(12));
  EXPECT_EQ(multi_chunk_hasher.finalize(), sha256(msg_bytes));
}

TEST(SHA256, IncrementalConcatEquivalence)
{
  std::vector<uint8_t> a = {0x10, 0x20, 0x30};
  std::vector<uint8_t> b = {0x40, 0x50, 0x60, 0x70};
  std::vector<uint8_t> ab = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70};

  auto a_bytes = std::as_bytes(std::span(a));
  auto b_bytes = std::as_bytes(std::span(b));
  auto ab_bytes = std::as_bytes(std::span(ab));

  SHA256Hasher incremental;
  incremental.update(a_bytes);
  incremental.update(b_bytes);
  EXPECT_EQ(incremental.finalize(), sha256(ab_bytes));

  SHA256Hasher with_leading_empty;
  with_leading_empty.update(std::span<const std::byte>{});
  with_leading_empty.update(ab_bytes);
  EXPECT_EQ(with_leading_empty.finalize(), sha256(ab_bytes));

  SHA256Hasher with_trailing_empty;
  with_trailing_empty.update(ab_bytes);
  with_trailing_empty.update(std::span<const std::byte>{});
  EXPECT_EQ(with_trailing_empty.finalize(), sha256(ab_bytes));

  SHA256Hasher with_middle_empty;
  with_middle_empty.update(a_bytes);
  with_middle_empty.update(std::span<const std::byte>{});
  with_middle_empty.update(b_bytes);
  EXPECT_EQ(with_middle_empty.finalize(), sha256(ab_bytes));
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

  // direct calculation
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
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  auto sig = ed25519_sign(sk, std::as_bytes(std::span(msg))).value();
  // NOLINTEND(bugprone-unchecked-optional-access)
  EXPECT_EQ(ed25519_verify(pk, sig, msg), true);
  EXPECT_EQ(ed25519_verify(pk, sig, msg_wrong), false);
}

// Catch an FFI UAF bug.
TEST(Ed25519, FFIBug)
{
  auto [pk, sk] = ed25519_keygen();
  uint64_t v = 0;
  auto msg = sha256(v);
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  auto sig = ed25519_sign(sk, msg).value();
  // NOLINTEND(bugprone-unchecked-optional-access)
  EXPECT_TRUE(ed25519_verify(pk, sig, msg));
}

TEST(Ed25519, BatchUsage)
{
  auto [pk1, sk1] = ed25519_keygen();
  auto [pk2, sk2] = ed25519_keygen();
  auto [pk3, sk3] = ed25519_keygen();

  std::string_view msg1 = "test message";
  std::string_view msg2 = "another message";
  std::string_view msg3 = "yet another message";

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  auto sig1 = ed25519_sign(sk1, msg1).value();
  auto sig2 = ed25519_sign(sk2, msg2).value();
  auto sig3 = ed25519_sign(sk3, msg3).value();
  // NOLINTEND(bugprone-unchecked-optional-access)

  std::array<std::span<const std::byte>, 3> msgs{std::as_bytes(std::span(msg1)),
                                                 std::as_bytes(std::span(msg2)),
                                                 std::as_bytes(
                                                     std::span(msg3))};
  EXPECT_EQ(ed25519_verify_batch(std::array{pk1, pk2, pk3},
                                 std::array{sig1, sig2, sig3},
                                 std::span<const std::span<const std::byte>>(
                                     msgs)),
            true);
}

TEST(Ed25519, BatchUsageEmpty)
{
  // An empty batch trivially verifies and must not pass null scratch pointers
  // to the FFI (vector::data() may be null when empty on libstdc++).
  EXPECT_EQ(ed25519_verify_batch(std::span<const Ed25519PublicKey>{},
                                 std::span<const Ed25519Signature>{},
                                 std::span<const std::span<const std::byte>>{}),
            true);
}

TEST(Ed25519, RandomKeygen)
{
  auto [pk, sk] = ed25519_keygen();
  auto [pk2, sk2] = ed25519_keygen();
  EXPECT_NE(pk, pk2);
  EXPECT_NE(sk, sk2);
}

TEST(Ed25519, SignReturnsNulloptForMismatchedKeypair)
{
  auto [pk, sk] = ed25519_keygen();
  std::string_view msg = "message signed with mismatched keypair";
  auto msg_bytes = std::as_bytes(std::span(msg));

  auto valid_sig = ed25519_sign(sk, msg_bytes);
  ASSERT_TRUE(valid_sig.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(ed25519_verify(pk, valid_sig.value(), msg));
  // NOLINTEND(bugprone-unchecked-optional-access)

  auto corrupted_sk = sk;
  corrupted_sk[0] ^= 0x01;

  auto invalid_sig = ed25519_sign(corrupted_sk, msg_bytes);
  EXPECT_FALSE(invalid_sig.has_value());
}

TEST(LtHash16, EmptyState)
{
  LtHash16 h;
  LtHash16::State expected{};
  EXPECT_EQ(h.state(), expected);
}

TEST(LtHash16, AddElement)
{
  std::vector<uint8_t> elem = {1, 2, 3, 4};
  LtHash16 h;
  h.add(elem);
  LtHash16::State zero{};
  EXPECT_NE(h.state(), zero);
}

TEST(LtHash16, Deterministic)
{
  std::vector<uint8_t> elem = {1, 2, 3, 4};
  LtHash16 h1, h2;
  h1.add(elem);
  h2.add(elem);
  EXPECT_EQ(h1, h2);
}

TEST(LtHash16, Commutativity)
{
  std::vector<uint8_t> a = {1, 2, 3};
  std::vector<uint8_t> b = {4, 5, 6};

  LtHash16 h1;
  h1.add(a);
  h1.add(b);

  LtHash16 h2;
  h2.add(b);
  h2.add(a);

  EXPECT_EQ(h1, h2);
}

TEST(LtHash16, AddRemoveIdentity)
{
  std::vector<uint8_t> elem = {10, 20, 30};
  LtHash16 h;
  LtHash16 original = h;

  h.add(elem);
  EXPECT_NE(h, original);
  h.remove(elem);
  EXPECT_EQ(h, original);
}

TEST(LtHash16, HomomorphicProperty)
{
  std::vector<uint8_t> a = {1, 2, 3};
  std::vector<uint8_t> b = {4, 5, 6};

  LtHash16 hab;
  hab.add(a);
  hab.add(b);

  LtHash16 ha;
  ha.add(a);
  LtHash16 hb;
  hb.add(b);

  EXPECT_EQ(ha + hb, hab);
}

TEST(LtHash16, OperatorMinusInverse)
{
  std::vector<uint8_t> a = {1, 2, 3};
  std::vector<uint8_t> b = {4, 5, 6};

  LtHash16 ha;
  ha.add(a);
  LtHash16 hb;
  hb.add(b);

  LtHash16 combined = ha + hb;
  EXPECT_EQ(combined - hb, ha);
  EXPECT_EQ(combined - ha, hb);
}

TEST(LtHash16, Reset)
{
  std::vector<uint8_t> elem = {1, 2, 3};
  LtHash16 h;
  h.add(elem);
  h.reset();
  EXPECT_EQ(h, LtHash16{});
}

TEST(LtHash16, DifferentInputsDifferentStates)
{
  std::vector<uint8_t> a = {1, 2, 3};
  std::vector<uint8_t> b = {3, 2, 1};

  LtHash16 ha, hb;
  ha.add(a);
  hb.add(b);
  EXPECT_NE(ha, hb);
}

TEST(LtHash16, MultisetProperty)
{
  std::vector<uint8_t> elem = {42};

  LtHash16 once;
  once.add(elem);

  LtHash16 twice;
  twice.add(elem);
  twice.add(elem);

  EXPECT_NE(once, twice);

  twice.remove(elem);
  EXPECT_EQ(once, twice);
}

// Defense-in-depth: both consensus paths (incremental ChangeSet and full
// rehash) now hash state elements through the piecewise overload, so they agree
// by construction. This test pins that streaming encoding to the raw contiguous
// add(BufView) primitive, so the documented format key || type || value cannot
// silently drift between them.
TEST(LtHash16, PiecewiseEqualsContiguous)
{
  std::vector<uint8_t> key = {1, 2, 3, 4};
  uint8_t type = 0x7;
  std::vector<uint8_t> value = {5, 6, 7, 8, 9};

  std::vector<uint8_t> contiguous;
  contiguous.insert(contiguous.end(), key.begin(), key.end());
  contiguous.push_back(type);
  contiguous.insert(contiguous.end(), value.begin(), value.end());

  LtHash16 piecewise;
  piecewise.add(key, type, value);

  LtHash16 whole;
  whole.add(contiguous);

  EXPECT_EQ(piecewise, whole);

  // remove() must mirror add() the same way.
  piecewise.remove(key, type, value);
  EXPECT_EQ(piecewise, LtHash16{});
}

// Empty key and empty value must still place the type byte between them, so the
// piecewise and contiguous encodings stay aligned at the boundaries.
TEST(LtHash16, PiecewiseEqualsContiguousEmptyPieces)
{
  std::vector<uint8_t> empty;
  uint8_t type = 0x0;

  std::vector<uint8_t> contiguous = {type};

  LtHash16 piecewise;
  piecewise.add(empty, type, empty);

  LtHash16 whole;
  whole.add(contiguous);

  EXPECT_EQ(piecewise, whole);
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
