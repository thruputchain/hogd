#include <gtest/gtest.h>
#include <string.h>

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/container/hex_array.h"

namespace hogd {

TEST(HexArray, Basic)
{
  HexArray h;
  EXPECT_EQ(h.str(), "");
  EXPECT_EQ(h.size(), 0);
}

TEST(HexArray, CreateFromString)
{
  std::string init = "0123456789abcdef";
  HexArray h(init);
  EXPECT_EQ(h.str(), init);
  EXPECT_EQ(h.size(), init.size());
}

TEST(HexArray, CreateFromStringOddLength)
{
  std::string init = "0123456789abcde";
  HexArray h(init);
  EXPECT_EQ(h.str(), init);
  EXPECT_EQ(h.size(), init.size());
}

TEST(HexArray, Get)
{
  std::string init = "0123456789abcdef";
  HexArray h(init);
  for (size_t i = 0; i < 16; i++) {
    EXPECT_EQ(h.Get(i), static_cast<std::byte>(i));
  }
}

TEST(HexArray, Set)
{
  std::string init = "0123456789abcdef";
  HexArray h(init);
  for (int i = 0; i < 16; i++) {
    h.Set(i, static_cast<std::byte>(0xf - i));
  }
  EXPECT_EQ(h.str(), "fedcba9876543210");
}

TEST(HexArray, AppendHexDigit)
{
  HexArray h;
  for (size_t i = 0; i < 16; i++) {
    h.Append(static_cast<std::byte>(i));
    EXPECT_EQ(h.size(), i + 1);
  }
  EXPECT_EQ(h.str(), "0123456789abcdef");
}

TEST(HexArray, AppendHexArray)
{
  HexArray h;
  std::string to_append_str = "123456";
  for (size_t i = 0; i < 16; i++) {
    HexArray to_append(to_append_str);
    h.Append(to_append);
    EXPECT_EQ(h.size(), (i + 1) * to_append_str.size());
  }
  std::string expected;
  for (size_t i = 0; i < 16; i++) {
    expected += to_append_str;
  }
  EXPECT_EQ(h.str(), expected);
}

TEST(HexArray, AppendHexArrayOddLength)
{
  HexArray h;
  std::string to_append_str = "123";
  for (size_t i = 0; i < 3; i++) {
    HexArray to_append(to_append_str);
    h.Append(to_append);
    EXPECT_EQ(h.size(), (i + 1) * to_append_str.size());
  }
  std::string expected;
  for (size_t i = 0; i < 3; i++) {
    expected += to_append_str;
  }
  EXPECT_EQ(h.str(), expected);
}

TEST(HexArray, RemoveFromBack)
{
  std::string init = "0123456789abcdef";
  HexArray h(init);

  h.RemoveFromBack(10);
  EXPECT_EQ(h.str(), "012345");
  EXPECT_EQ(h.size(), 6);
}

TEST(HexArray, RemoveFromBackOddLength)
{
  std::string init = "0123456789abcdef";
  HexArray h(init);

  h.RemoveFromBack(9);
  EXPECT_EQ(h.str(), "0123456");
  EXPECT_EQ(h.size(), 7);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
