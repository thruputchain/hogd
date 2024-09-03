#include <string>
#include <string_view>
#include <vector>

#include "hogd/base/string.h"
#include <gtest/gtest.h>

namespace hogd {

TEST(StringTest, BasicSplitting)
{
  std::string str("/foo/bar/baz/");
  std::vector<std::string_view> tokens = Split(str, '/');
  EXPECT_EQ(tokens.size(), 5);
  EXPECT_EQ(tokens[0], "");
  EXPECT_EQ(tokens[1], "foo");
  EXPECT_EQ(tokens[2], "bar");
  EXPECT_EQ(tokens[3], "baz");
  EXPECT_EQ(tokens[4], "");

  str = "foo.bar.baz";
  tokens = Split(str, '.');
  EXPECT_EQ(tokens.size(), 3);
  EXPECT_EQ(tokens[0], "foo");
  EXPECT_EQ(tokens[1], "bar");
  EXPECT_EQ(tokens[2], "baz");
}

TEST(StringTest, LimitSplitting)
{
  std::string str("the quick brown fox jumped over the moon");
  std::vector<std::string_view> tokens = Split(str, ' ', 3);
  EXPECT_EQ(tokens.size(), 4);
  EXPECT_EQ(tokens[0], "the");
  EXPECT_EQ(tokens[1], "quick");
  EXPECT_EQ(tokens[2], "brown");
  EXPECT_EQ(tokens[3], "fox jumped over the moon");
}

} // namespace hogd
