#include <gtest/gtest.h>

#include <span>

#include <vector>
#include <x23/serialize.h>
#include <x23/serialize_cxs.h>
#include <x23/utility.h>
#include <x23/vec.h>

#include "tests/xdrtest.hh"

namespace x23 {

TEST(Deserialize, Vector)
{
  x23::Vec<TxEventLog, 0xffffffff> vec;

  // Full buf in elts, from commented hex string
  // ceb73749c899693706ede1e30c9929b3fd5dd926163831c2fb8bd41e6efb112600000001746573745f6164647265737300000000000000000000000000000000000000000000000301020300
  x23::Buf buf = {
      0xce, 0xb7, 0x37, 0x49, 0xc8, 0x99, 0x69, 0x37, 0x06, 0xed, 0xe1,
      0xe3, 0x0c, 0x99, 0x29, 0xb3, 0xfd, 0x5d, 0xd9, 0x26, 0x16, 0x38,
      0x31, 0xc2, 0xfb, 0x8b, 0xd4, 0x1e, 0x6e, 0xfb, 0x11, 0x26, 0x00,
      0x00, 0x00, 0x01, 0x74, 0x65, 0x73, 0x74, 0x5f, 0x61, 0x64, 0x64,
      0x72, 0x65, 0x73, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x01, 0x02, 0x03, 0x00,
  };

  EXPECT_THROW(deserialize(&vec, BufView(buf)), x23::XdrBadInput);
}

// CXS BoundedStr deserialization must reject input where the decoded
// length exceeds the remaining span size (heap-buffer-overflow found
// by fuzz_deserialize).
TEST(Deserialize, CxsStrTooShort)
{
  uint8_t input[] = {0x0a}; // CXS-encodes length 10, but only 0 bytes follow
  std::span<const uint8_t> sp(input, sizeof(input));
  x23::Str val;
  EXPECT_THROW(x23::deserialize_cxs(&val, sp), x23::CxsBadInput);
}

// CXS reserved header bytes: any byte > 62 where the bottom 6 bits
// are not all 1s (i.e., not one of {63, 127, 191, 255}) is a reserved
// encoding per the CXS spec.  load_cxs_uint and load_cxs_int guard
// this with x23assert but no throw, so in NDEBUG builds these are
// silently accepted.  These tests verify the deserializer rejects them.
TEST(Deserialize, CxsReservedUintHeader)
{
  // Byte 64 = 0b01000000: s=1, v=0 — reserved (v should be 63)
  // Followed by enough bytes that the code won't fail on size checks.
  uint8_t input[] = {64, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  std::span<const uint8_t> sp(input, sizeof(input));
  uint32_t val = 0;
  EXPECT_THROW(x23::deserialize_cxs(&val, sp), x23::XdrException);
}

TEST(Deserialize, CxsReservedIntHeader)
{
  // Byte 100 = 0b01100100: s=1, v=36 — reserved (v should be 63)
  uint8_t input[] = {100, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  std::span<const uint8_t> sp(input, sizeof(input));
  int32_t val = 0;
  EXPECT_THROW(x23::deserialize_cxs(&val, sp), x23::XdrException);
}

TEST(Deserialize, CxsReservedHeaderVariousBytes)
{
  // All of these have bottom 6 bits != 63 and value > 62, so they are
  // reserved.  Each should be rejected.
  for (uint8_t byte : {64, 65, 100, 126, 128, 190, 192, 200, 254}) {
    uint8_t input[] = {byte, 0, 0, 0, 0, 0, 0, 0, 0};
    std::span<const uint8_t> sp(input, sizeof(input));
    uint64_t uval = 0;
    EXPECT_THROW(x23::deserialize_cxs(&uval, sp), x23::XdrException)
        << "byte " << int(byte) << " should be rejected for uint";
    int64_t ival = 0;
    EXPECT_THROW(x23::deserialize_cxs(&ival, sp), x23::XdrException)
        << "byte " << int(byte) << " should be rejected for int";
  }
}

TEST(Deserialize, OpaqueVectorTooLarge)
{
  x23::Vec<uint8_t, 5> vec;

  std::vector<uint8_t> input = {0x00, 0x00, 0x00, 0x09};
  input.insert(input.end(), {0, 1, 2, 3, 4, 5, 6, 7, 8});
  EXPECT_THROW(deserialize(&vec, BufView(input)), x23::XdrBadInput);

  x23::Vec<uint8_t, 4> vec2;

  std::vector<uint8_t> input = {0x00, 0x00, 0x00, 0x08};
  input.insert(input.end(), {0, 1, 2, 3, 4, 5, 6, 7});
  EXPECT_THROW(deserialize(&vec2, BufView(input)), x23::XdrBadInput);
}

TEST(Deserialize, VectorTooLarge)
{
  x23::Vec<uint32_t, 1> vec;

  // valid encoding for a vector of length 2
  std::vector<uint8_t> input = {0x00, 0x00, 0x00, 0x02};
  input.insert(input.end(), {0, 1, 2, 3, 4, 5, 6, 7});
  EXPECT_THROW(deserialize(&vec, BufView(input)), x23::XdrBadInput);
}

TEST(Deserialize, CxsVectorTooLarge)
{
  x23::Vec<uint32_t, 2> vec;

  // CXS uses little-endian size prefix
  std::vector<uint8_t> input = {0x03, 0x00, 0x00, 0x00};
  input.insert(input.end(), {0, 1, 2, 3, 4, 5, 6, 7});
  std::span<const uint8_t> sp(input);
  EXPECT_THROW(x23::deserialize_cxs(&vec, sp), x23::CxsBadInput);
}

} // namespace x23
