#include <gtest/gtest.h>

#include <cstdlib>

#include "hogd/base/cli.h"

namespace hogd {

TEST(CliTest, ParsePortAcceptsFullRange)
{
  EXPECT_EQ(parse_port("0"), 0);
  EXPECT_EQ(parse_port("80"), 80);
  EXPECT_EQ(parse_port("8080"), 8080);
  EXPECT_EQ(parse_port("65535"), 65535);
}

TEST(CliTest, ParseIntegerOrDieAcceptsFullRange)
{
  EXPECT_EQ(parse_integer_or_die<uint64_t>("gas limit", "0"), 0U);
  EXPECT_EQ(parse_integer_or_die<int32_t>("offset", "-5"), -5);
}

TEST(CliTest, ParseIntegerOrDieAcceptsHex)
{
  EXPECT_EQ(parse_integer_or_die<uint64_t>("network id", "ff", 16), 0xffU);
  EXPECT_EQ(parse_integer_or_die<uint64_t>("network id", "deadbeef", 16),
            0xdeadbeefU);
  // std::stoul accepted a 0x prefix for base 16; preserve that.
  EXPECT_EQ(parse_integer_or_die<uint64_t>("network id", "0xff", 16), 0xffU);
}

TEST(CliTest, ParseIntegerOrDieAutoDetectsBase)
{
  EXPECT_EQ(parse_integer_or_die<size_t>("ram bytes", "10000000000", 0),
            10000000000U);
  EXPECT_EQ(parse_integer_or_die<size_t>("ram bytes", "0x40", 0), 0x40U);
  // Deliberately NOT octal, unlike std::stoul with base 0.
  EXPECT_EQ(parse_integer_or_die<size_t>("ram bytes", "0755", 0), 755U);
}

TEST(CliTest, ParseHostPortAcceptsJoinedAddress)
{
  x23::HostPort hp =
      parse_host_port_or_die("mempool address", "127.0.0.1:8080");
  EXPECT_EQ(std::string(hp.host.begin(), hp.host.end()), "127.0.0.1");
  EXPECT_EQ(std::string(hp.port.begin(), hp.port.end()), "8080");
}

using CliDeathTest = ::testing::Test;

TEST(CliDeathTest, ParseHostPortRejectsMalformed)
{
  EXPECT_EXIT(parse_host_port_or_die("mempool address", "not-an-address"),
              ::testing::ExitedWithCode(EXIT_FAILURE),
              "Invalid mempool address: not-an-address");
}

// The hand-rolled std::stoul parsers this replaces threw an *uncaught*
// std::invalid_argument here (and std::out_of_range on a huge value): stoul
// throws before their own range check runs, so the daemon aborted on an
// uncaught exception instead of reporting the bad flag.
TEST(CliDeathTest, ParsePortRejectsNonNumeric)
{
  EXPECT_EXIT(parse_port("abc"), ::testing::ExitedWithCode(EXIT_FAILURE),
              "Invalid port: abc");
}

TEST(CliDeathTest, ParsePortRejectsHugeValue)
{
  EXPECT_EXIT(parse_port("99999999999999999999"),
              ::testing::ExitedWithCode(EXIT_FAILURE),
              "Invalid port: 99999999999999999999");
}

TEST(CliDeathTest, ParsePortRejectsTrailingCharacters)
{
  EXPECT_EXIT(parse_port("80abc"), ::testing::ExitedWithCode(EXIT_FAILURE),
              "Invalid port: 80abc");
}

TEST(CliDeathTest, ParsePortRejectsOutOfRange)
{
  EXPECT_EXIT(parse_port("65536"), ::testing::ExitedWithCode(EXIT_FAILURE),
              "Invalid port: 65536");
}

TEST(CliDeathTest, ParsePortRejectsNegative)
{
  EXPECT_EXIT(parse_port("-1"), ::testing::ExitedWithCode(EXIT_FAILURE),
              "Invalid port: -1");
}

TEST(CliDeathTest, ParsePortRejectsEmpty)
{
  EXPECT_EXIT(parse_port(""), ::testing::ExitedWithCode(EXIT_FAILURE),
              "Invalid port: ");
}

TEST(CliDeathTest, ParseIntegerOrDieReportsLabel)
{
  EXPECT_EXIT(parse_integer_or_die<uint16_t>("metrics port", "nope"),
              ::testing::ExitedWithCode(EXIT_FAILURE),
              "Invalid metrics port: nope");
}

TEST(CliDeathTest, DiePrintsMessage)
{
  EXPECT_EXIT(die("something went wrong"),
              ::testing::ExitedWithCode(EXIT_FAILURE), "something went wrong");
}

} // namespace hogd
