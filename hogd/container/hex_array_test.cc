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
  EXPECT_TRUE(h.empty());
}

TEST(HexArray, EmptyString)
{
  HexArray h("");
  EXPECT_EQ(h.str(), "");
  EXPECT_EQ(h.size(), 0);
  EXPECT_TRUE(h.empty());
}

TEST(HexArray, CreateFromString)
{
  std::string init = "0123456789abcdef";
  HexArray h(init);
  EXPECT_EQ(h.str(), init);
  EXPECT_EQ(h.size(), init.size());
  EXPECT_FALSE(h.empty());
}

TEST(HexArray, CreateFromStringSingleDigit)
{
  std::string init = "a";
  HexArray h(init);
  EXPECT_EQ(h.str(), init);
  EXPECT_EQ(h.size(), init.size());
  EXPECT_FALSE(h.empty());
}

TEST(HexArray, CreateFromStringOddLength)
{
  std::string init = "0123456789abcde";
  HexArray h(init);
  EXPECT_EQ(h.str(), init);
  EXPECT_EQ(h.size(), init.size());
  EXPECT_FALSE(h.empty());
}

TEST(HexArray, CreateFromByteVector)
{
  std::vector<std::byte> bytes = {std::byte{0x01}, std::byte{0x23},
                                  std::byte{0x45}, std::byte{0x67},
                                  std::byte{0x89}, std::byte{0xab},
                                  std::byte{0xcd}, std::byte{0xef}};
  HexArray h(bytes);
  EXPECT_EQ(h.str(), "0123456789abcdef");
  EXPECT_EQ(h.ToByteVector(), bytes);
  EXPECT_EQ(h.size(), bytes.size() * 2);
  EXPECT_FALSE(h.empty());
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
    EXPECT_FALSE(h.empty());
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
    EXPECT_FALSE(h.empty());
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
    EXPECT_FALSE(h.empty());
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
  EXPECT_FALSE(h.empty());
}

TEST(HexArray, RemoveFromBackOddLength)
{
  std::string init = "0123456789abcdef";
  HexArray h(init);

  h.RemoveFromBack(9);
  EXPECT_EQ(h.str(), "0123456");
  EXPECT_EQ(h.size(), 7);
  EXPECT_FALSE(h.empty());
}

TEST(HexArray, Equal)
{
  HexArray a("0123456789abcdef");
  HexArray b("0123456789abcdef");

  EXPECT_TRUE(a == b);
}

TEST(HexArray, EqualOdd)
{
  HexArray a("0123456789abcde");
  HexArray b("0123456789abcde");

  EXPECT_TRUE(a == b);
}

TEST(HexArray, NotEqual)
{
  HexArray a("0123456789abcdef");
  HexArray b("0123456789abcde");

  EXPECT_FALSE(a == b);
}

TEST(HexArray, NotEqualOddLastDigitIsDifferent)
{
  HexArray a("0123456789abcde");
  HexArray b("0123456789abcdf");

  EXPECT_FALSE(a == b);
}

TEST(HexArray, LessThan)
{
  HexArray a("0123456789abcdef");
  HexArray b("0123456789abcdff");

  EXPECT_TRUE(a < b);
}

TEST(HexArray, LessThanShorterLength)
{
  HexArray a("5123456789abcde");
  HexArray b("0123456789abcdef");

  EXPECT_TRUE(a < b);
}

TEST(HexArray, NotLessThan)
{
  HexArray a("0123456789abcdef");
  HexArray b("0123456789abcdef");

  EXPECT_FALSE(a < b);
}

TEST(HexArray, NotLessThanBecauseBigger)
{
  HexArray a("0123456789abcdff");
  HexArray b("0123456789abcdef");

  EXPECT_FALSE(a < b);
}

TEST(HexArray, NotLessThanBecauseLongerLength)
{
  HexArray a("0123456789abcdef");
  HexArray b("5123456789abcde");

  EXPECT_FALSE(a < b);
}

TEST(HexArray, GetSubarrayEntireArray)
{
  HexArray a("0123456789abcdef");
  HexArray subarray = a.GetSubarray(0, a.size());

  EXPECT_EQ(subarray, a);
  EXPECT_EQ(subarray.size(), a.size());
  EXPECT_FALSE(subarray.empty());
}
TEST(HexArray, GetSubarrayEmptyArray)
{
  HexArray a("0123456789abcdef");
  HexArray subarray = a.GetSubarray(5, 5);
  HexArray expected;

  EXPECT_EQ(subarray, expected);
  EXPECT_EQ(subarray.size(), expected.size());
  EXPECT_TRUE(subarray.empty());
}

TEST(HexArray, GetSubarraySingleDigit)
{
  HexArray a("0123456789abcdef");
  HexArray subarray = a.GetSubarray(5, 6);
  HexArray expected("5");

  EXPECT_EQ(subarray, expected);
  EXPECT_EQ(subarray.size(), expected.size());
  EXPECT_FALSE(subarray.empty());
}

TEST(HexArray, GetSubarrayMiddle)
{
  HexArray a("0123456789abcdef");
  HexArray subarray = a.GetSubarray(7, 13);
  HexArray expected("789abc");

  EXPECT_EQ(subarray, expected);
  EXPECT_EQ(subarray.size(), expected.size());
  EXPECT_FALSE(subarray.empty());
}

TEST(HexArray, GetSubarrayClipBeginning)
{
  HexArray a("0123456789abcdef");
  HexArray subarray = a.GetSubarray(7, a.size());
  HexArray expected("789abcdef");

  EXPECT_EQ(subarray, expected);
  EXPECT_EQ(subarray.size(), expected.size());
  EXPECT_FALSE(subarray.empty());
}

TEST(HexArray, GetSubarrayClipEnd)
{
  HexArray a("0123456789abcdef");
  HexArray subarray = a.GetSubarray(0, 13);
  HexArray expected("0123456789abc");

  EXPECT_EQ(subarray, expected);
  EXPECT_EQ(subarray.size(), expected.size());
  EXPECT_FALSE(subarray.empty());
}

TEST(HexArray, ToByteVector)
{
  HexArray a("0123456789abcdef");
  std::vector<std::byte> bytes = a.ToByteVector();
  std::vector<std::byte> expected = {std::byte{0x01}, std::byte{0x23},
                                     std::byte{0x45}, std::byte{0x67},
                                     std::byte{0x89}, std::byte{0xab},
                                     std::byte{0xcd}, std::byte{0xef}};

  EXPECT_EQ(bytes, expected);
}

TEST(HexArray, ToByteVectorEmpty)
{
  HexArray a;
  std::vector<std::byte> bytes = a.ToByteVector();
  std::vector<std::byte> expected = {};

  EXPECT_EQ(bytes, expected);
}

TEST(HexArray, ToByteVectorSingleDigit)
{
  HexArray a("a");
  std::vector<std::byte> bytes = a.ToByteVector();
  std::vector<std::byte> expected = {std::byte{0xa0}};

  EXPECT_EQ(bytes, expected);
}

TEST(HexArray, ToByteVectorOddLength)
{
  HexArray a("0123456789abcde");
  std::vector<std::byte> bytes = a.ToByteVector();
  std::vector<std::byte> expected = {std::byte{0x01}, std::byte{0x23},
                                     std::byte{0x45}, std::byte{0x67},
                                     std::byte{0x89}, std::byte{0xab},
                                     std::byte{0xcd}, std::byte{0xe0}};

  EXPECT_EQ(bytes, expected);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
