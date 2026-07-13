#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

#include "hogd/base/cli.h"

namespace hogd {

TEST(CliTest, TryParsePortAcceptsFullRange)
{
  EXPECT_EQ(try_parse_port("0"), 0);
  EXPECT_EQ(try_parse_port("80"), 80);
  EXPECT_EQ(try_parse_port("8080"), 8080);
  EXPECT_EQ(try_parse_port("65535"), 65535);
}

// The hand-rolled std::stoul parsers this replaces threw an *uncaught*
// std::invalid_argument on non-numeric input (and std::out_of_range on a huge
// value): stoul throws before their own range check runs, so the daemon died on
// an uncaught exception rather than reporting which flag was bad.
TEST(CliTest, TryParsePortRejectsBadInput)
{
  EXPECT_EQ(try_parse_port("abc"), std::nullopt);
  EXPECT_EQ(try_parse_port("80abc"), std::nullopt);
  EXPECT_EQ(try_parse_port("65536"), std::nullopt);
  EXPECT_EQ(try_parse_port("99999999999999999999"), std::nullopt);
  EXPECT_EQ(try_parse_port("-1"), std::nullopt);
  EXPECT_EQ(try_parse_port(""), std::nullopt);
  EXPECT_EQ(try_parse_port(" 80"), std::nullopt);
}

TEST(CliTest, TryParseIntegerAcceptsFullRange)
{
  EXPECT_EQ(try_parse_integer<uint64_t>("0"), 0U);
  EXPECT_EQ(try_parse_integer<int32_t>("-5"), -5);
  EXPECT_EQ(try_parse_integer<size_t>("10000000000"), 10000000000U);
}

TEST(CliTest, TryParseIntegerAcceptsHex)
{
  EXPECT_EQ(try_parse_integer<uint64_t>("ff", 16), 0xffU);
  EXPECT_EQ(try_parse_integer<uint64_t>("deadbeef", 16), 0xdeadbeefU);
  // std::stoul accepted a 0x prefix for base 16; preserve that.
  EXPECT_EQ(try_parse_integer<uint64_t>("0xff", 16), 0xffU);
}

TEST(CliTest, TryParseIntegerAutoDetectsBase)
{
  EXPECT_EQ(try_parse_integer<size_t>("4096", 0), 4096U);
  EXPECT_EQ(try_parse_integer<size_t>("0x40", 0), 0x40U);
  // Deliberately NOT octal, unlike std::stoul with base 0.
  EXPECT_EQ(try_parse_integer<size_t>("0755", 0), 755U);
}

TEST(CliTest, TryParseIntegerRejectsBadInput)
{
  EXPECT_EQ(try_parse_integer<uint64_t>("nope"), std::nullopt);
  EXPECT_EQ(try_parse_integer<uint64_t>("12x"), std::nullopt);
  EXPECT_EQ(try_parse_integer<uint8_t>("256"), std::nullopt);
  // Hex digits are not valid in the default base.
  EXPECT_EQ(try_parse_integer<uint64_t>("ff"), std::nullopt);
}

} // namespace hogd
