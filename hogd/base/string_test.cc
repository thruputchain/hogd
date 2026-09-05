#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "hogd/base/network_hash.h"
#include "hogd/base/span.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"

namespace hogd {

TEST(StringTest, BasicSplitting)
{
  std::string str("/foo/bar/baz/");
  std::vector<std::string_view> tokens = split(str, '/');
  EXPECT_EQ(tokens.size(), 5U);
  EXPECT_EQ(tokens[0], "");
  EXPECT_EQ(tokens[1], "foo");
  EXPECT_EQ(tokens[2], "bar");
  EXPECT_EQ(tokens[3], "baz");
  EXPECT_EQ(tokens[4], "");

  str = "foo.bar.baz";
  tokens = split(str, '.');
  EXPECT_EQ(tokens.size(), 3U);
  EXPECT_EQ(tokens[0], "foo");
  EXPECT_EQ(tokens[1], "bar");
  EXPECT_EQ(tokens[2], "baz");
}

TEST(StringTest, LimitSplitting)
{
  std::string str("the quick brown fox jumped over the moon");
  std::vector<std::string_view> tokens = split(str, ' ', 3);
  EXPECT_EQ(tokens.size(), 4U);
  EXPECT_EQ(tokens[0], "the");
  EXPECT_EQ(tokens[1], "quick");
  EXPECT_EQ(tokens[2], "brown");
  EXPECT_EQ(tokens[3], "fox jumped over the moon");
}

TEST(StringTest, Encode)
{
  std::string test_str("hello world!");
  EXPECT_EQ(encode_hex(byte_view(test_str.data(), test_str.size())),
            "68656c6c6f20776f726c6421");
  auto decoded = decode_hex("68656c6c6f20776f726c6421");
  EXPECT_EQ(std::memcmp(decoded.data(), test_str.data(), test_str.size()), 0);
  EXPECT_EQ(encode_base32(byte_view(test_str.data(), test_str.size())),
            "NBSWY3DPEB3W64TMMQQQ");
  EXPECT_EQ(encode_base64(byte_view(test_str.data(), test_str.size())),
            "aGVsbG8gd29ybGQh");
}

TEST(StringTest, DecodeHexEvenLength)
{
  // Even-length hex string should decode correctly
  auto result = decode_hex("abcd");
  ASSERT_EQ(result.size(), 2U);
  EXPECT_EQ(result[0], std::byte{0xab});
  EXPECT_EQ(result[1], std::byte{0xcd});
}

TEST(StringTest, DecodeHexOddLengthDeath)
{
  // Odd-length hex string should CHECK-fail (out-of-bounds read)
  EXPECT_DEATH(decode_hex("abc"), "");
}

TEST(NetworkHashTest, ParsesAndFormatsCanonicalHex)
{
  constexpr std::string_view kText = "000102030405060708090a0b0c0d0e0f"
                                     "101112131415161718191a1b1c1d1e1f";
  const auto hash = parse_network_hash(kText);
  ASSERT_TRUE(hash.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(format_network_hash(hash.value()), kText);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST(NetworkHashTest, RejectsNonCanonicalLengthsAndInvalidHex)
{
  EXPECT_FALSE(parse_network_hash("1").has_value());
  EXPECT_FALSE(parse_network_hash(std::string(64, 'g')).has_value());
  EXPECT_FALSE(parse_network_hash(std::string(66, '0')).has_value());
}

TEST(PrintIecRateTest, DividesByElapsedSeconds)
{
  EXPECT_EQ(print_iec_rate(2UL << 20, 2.0), "1MiB");
  EXPECT_EQ(print_iec_rate(1UL << 30, 1.0, true), "1 GiB");
}

// A zero interval is legal for steady_clock, so the quotient goes non-finite:
// infinity when bytes were written, NaN when none were. Converting either to
// size_t is undefined behavior (#2900), so both must report zero. Under
// -fsanitize=float-cast-overflow an unguarded implementation aborts here.
TEST(PrintIecRateTest, ZeroElapsedReportsZeroInsteadOfConvertingNonFinite)
{
  EXPECT_EQ(print_iec_rate(1UL << 30, 0.0), "0B");
  EXPECT_EQ(print_iec_rate(0, 0.0), "0B");
  EXPECT_EQ(print_iec_rate(1UL << 30, 0.0, true), "0 B");
}

// elapsed() is steady_clock-backed and cannot go backwards, but the helper is
// public API and must not convert a negative quotient to an unsigned type.
TEST(PrintIecRateTest, NegativeElapsedReportsZero)
{
  EXPECT_EQ(print_iec_rate(1UL << 30, -1.0), "0B");
}

// A finite quotient can still exceed SIZE_MAX for a small enough interval.
TEST(PrintIecRateTest, SaturatesRatesTooLargeForSizeT)
{
  EXPECT_EQ(print_iec_rate(std::numeric_limits<size_t>::max(), 1e-9),
            print_iec_bytes(std::numeric_limits<size_t>::max()));
}

} // namespace hogd
