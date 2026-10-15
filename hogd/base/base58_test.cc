#include "hogd/base/base58.h"

#include <array>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

namespace hogd {
namespace {

TEST(Base58, FixedIdentifiersPreserveBytes)
{
  const std::array<std::uint8_t, 32> zero{};
  EXPECT_EQ(encode_base58(zero), std::string(32, '1'));
  EXPECT_EQ(decode_base58_fixed<32>(std::string(32, '1')), zero);
  std::array<std::uint8_t, 32> ones{};
  ones.back() = 1;
  EXPECT_EQ(encode_base58(ones), std::string(31, '1') + "2");
  EXPECT_EQ(decode_base58_fixed<32>(encode_base58(ones)), ones);
  std::array<std::uint8_t, 32> ff{};
  ff.fill(255);
  constexpr auto text = "JEKNVnkbo3jma5nREBBJCDoXFVeKkD56V3xKrvRmWxFG";
  EXPECT_EQ(encode_base58(ff), text);
  EXPECT_EQ(decode_base58_fixed<32>(text), ff);
}

TEST(Base58, RejectsInvalidAndWrongWidthText)
{
  for (const auto &text :
       {std::string{}, std::string(31, '1'), std::string(33, '1'),
        std::string(45, 'z'), std::string(64, 'a'), "0x" + std::string(64, '0'),
        std::string(31, '1') + "0", std::string(31, '1') + "O",
        std::string(31, '1') + "I", std::string(31, '1') + "l",
        " " + std::string(32, '1'), std::string(32, '1') + "\n"}) {
    EXPECT_FALSE(decode_base58_fixed<32>(text).has_value());
  }
}

TEST(Base58, CaseIsSignificant)
{
  EXPECT_NE(decode_base58_fixed<1>("a"), decode_base58_fixed<1>("A"));
}

TEST(Base58, SharedProtocolVectors)
{
  const std::array<std::uint8_t, 4> leading{0, 0, 1, 255};
  EXPECT_EQ(encode_base58(leading), "119p");
  EXPECT_EQ(decode_base58_fixed<4>("119p"), leading);
  std::array<std::uint8_t, 32> increasing{};
  for (std::size_t i = 0; i < increasing.size(); ++i) {
    increasing[i] = static_cast<std::uint8_t>(i);
  }
  constexpr auto text = "1thX6LZfHDZZKUs92febYZhYRcXddmzfzF2NvTkPNE";
  EXPECT_EQ(encode_base58(increasing), text);
  EXPECT_EQ(decode_base58_fixed<32>(text), increasing);
}

} // namespace
} // namespace hogd
