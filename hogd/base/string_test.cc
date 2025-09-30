#include <gtest/gtest.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hogd/base/span.h"
#include "hogd/base/string.h"

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

} // namespace hogd
