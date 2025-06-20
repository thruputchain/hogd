#include <cstddef>
#include <gtest/gtest.h>

#include <vector>

#include "hogd/base/types.h"
#include "hogd/container/merkle/hex_array.h"
#include "types.hh"

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
  EXPECT_EQ(h.to_byte_vector(), bytes);
  EXPECT_EQ(h.size(), bytes.size() * 2);
  EXPECT_FALSE(h.empty());
}

TEST(HexArray, CreateFromSHA256Hash)
{
  SHA256Hash hash;
  for (size_t i = 0; i < hash.size(); i++) {
    auto hex_digit = static_cast<unsigned char>(i % 16);
    hash[i] = (hex_digit << 4) | hex_digit;
  }
  HexArray h(hash);
  EXPECT_EQ(h.str(),
            "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
  EXPECT_EQ(h.size(), hash.size() * 2);
  EXPECT_FALSE(h.empty());
}

TEST(HexArray, Get)
{
  std::string init = "0123456789abcdef";
  HexArray h(init);
  for (size_t i = 0; i < h.size(); i++) {
    EXPECT_EQ(h.get(i), static_cast<std::byte>(i));
  }
}

TEST(HexArray, Set)
{
  std::string init = "0123456789abcdef";
  HexArray h(init);
  for (size_t i = 0; i < h.size(); i++) {
    h.set(i, static_cast<std::byte>(0xF - i));
  }
  EXPECT_EQ(h.str(), "fedcba9876543210");
}

TEST(HexArray, AppendHexDigit)
{
  HexArray h;
  for (size_t i = 0; i < 16; i++) {
    h.append(static_cast<std::byte>(i));
    EXPECT_EQ(h.size(), i + 1);
    EXPECT_FALSE(h.empty());
  }
  EXPECT_EQ(h.str(), "0123456789abcdef");
}

TEST(HexArray, AppendHexArray)
{
  HexArray h;
  std::string to_append_str = "123456";
  for (size_t i = 0; i < 10; i++) {
    HexArray to_append(to_append_str);
    h.append(to_append);
    EXPECT_EQ(h.size(), (i + 1) * to_append_str.size());
    EXPECT_FALSE(h.empty());
  }
  std::string expected;
  for (size_t i = 0; i < 10; i++) {
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
    h.append(to_append);
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

  h.remove_from_back(10);
  EXPECT_EQ(h.str(), "012345");
  EXPECT_EQ(h.size(), 6);
  EXPECT_FALSE(h.empty());
}

TEST(HexArray, RemoveFromBackOddLength)
{
  std::string init = "0123456789abcdef";
  HexArray h(init);

  h.remove_from_back(9);
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
  HexArray subarray = a.get_subarray(0, a.size());

  EXPECT_EQ(subarray, a);
  EXPECT_EQ(subarray.size(), a.size());
  EXPECT_FALSE(subarray.empty());
}
TEST(HexArray, GetSubarrayEmptyArray)
{
  HexArray a("0123456789abcdef");
  HexArray subarray = a.get_subarray(5, 5);
  HexArray expected;

  EXPECT_EQ(subarray, expected);
  EXPECT_EQ(subarray.size(), expected.size());
  EXPECT_TRUE(subarray.empty());
}

TEST(HexArray, GetSubarraySingleDigit)
{
  HexArray a("0123456789abcdef");
  HexArray subarray = a.get_subarray(5, 6);
  HexArray expected("5");

  EXPECT_EQ(subarray, expected);
  EXPECT_EQ(subarray.size(), expected.size());
  EXPECT_FALSE(subarray.empty());
}

TEST(HexArray, GetSubarrayMiddle)
{
  HexArray a("0123456789abcdef");
  HexArray subarray = a.get_subarray(7, 13);
  HexArray expected("789abc");

  EXPECT_EQ(subarray, expected);
  EXPECT_EQ(subarray.size(), expected.size());
  EXPECT_FALSE(subarray.empty());
}

TEST(HexArray, GetSubarrayClipBeginning)
{
  HexArray a("0123456789abcdef");
  HexArray subarray = a.get_subarray(7, a.size());
  HexArray expected("789abcdef");

  EXPECT_EQ(subarray, expected);
  EXPECT_EQ(subarray.size(), expected.size());
  EXPECT_FALSE(subarray.empty());
}

TEST(HexArray, GetSubarrayClipEnd)
{
  HexArray a("0123456789abcdef");
  HexArray subarray = a.get_subarray(0, 13);
  HexArray expected("0123456789abc");

  EXPECT_EQ(subarray, expected);
  EXPECT_EQ(subarray.size(), expected.size());
  EXPECT_FALSE(subarray.empty());
}

TEST(HexArray, ToByteVector)
{
  HexArray a("0123456789abcdef");
  std::vector<std::byte> bytes = a.to_byte_vector();
  std::vector<std::byte> expected = {std::byte{0x01}, std::byte{0x23},
                                     std::byte{0x45}, std::byte{0x67},
                                     std::byte{0x89}, std::byte{0xab},
                                     std::byte{0xcd}, std::byte{0xef}};

  EXPECT_EQ(bytes, expected);
}

TEST(HexArray, ToByteVectorEmpty)
{
  HexArray a;
  std::vector<std::byte> bytes = a.to_byte_vector();
  std::vector<std::byte> expected = {};

  EXPECT_EQ(bytes, expected);
}

TEST(HexArray, ToByteVectorSingleDigit)
{
  HexArray a("a");
  std::vector<std::byte> bytes = a.to_byte_vector();
  std::vector<std::byte> expected = {std::byte{0xa0}};

  EXPECT_EQ(bytes, expected);
}

TEST(HexArray, ToByteVectorOddLength)
{
  HexArray a("0123456789abcde");
  std::vector<std::byte> bytes = a.to_byte_vector();
  std::vector<std::byte> expected = {std::byte{0x01}, std::byte{0x23},
                                     std::byte{0x45}, std::byte{0x67},
                                     std::byte{0x89}, std::byte{0xab},
                                     std::byte{0xcd}, std::byte{0xe0}};

  EXPECT_EQ(bytes, expected);
}

TEST(HexArray, clear)
{
  std::string init = "01234";
  HexArray a(init);
  EXPECT_EQ(a.size(), init.size());

  a.clear();
  EXPECT_TRUE(a.empty());
  EXPECT_EQ(a.str(), "");
}

TEST(HexArray, clearEmpty)
{
  HexArray a;
  EXPECT_TRUE(a.empty());

  a.clear();
  EXPECT_TRUE(a.empty());
  EXPECT_EQ(a.str(), "");
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
