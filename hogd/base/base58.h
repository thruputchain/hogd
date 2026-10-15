// -*-C++-*-
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hogd {

inline constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

// Bitcoin/Solana alphabet; leading zero bytes are preserved as leading '1's.
inline std::string
encode_base58(std::span<const std::uint8_t> bytes)
{
  std::size_t zeros = 0;
  while (zeros < bytes.size() && bytes[zeros] == 0) {
    ++zeros;
  }
  std::vector<std::uint8_t> digits;
  for (auto byte : bytes.subspan(zeros)) {
    unsigned carry = byte;
    for (auto &digit : digits) {
      carry += static_cast<unsigned>(digit) * 256;
      digit = static_cast<std::uint8_t>(carry % 58);
      carry /= 58;
    }
    while (carry != 0) {
      digits.push_back(static_cast<std::uint8_t>(carry % 58));
      carry /= 58;
    }
  }
  std::string result(zeros, '1');
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    result.push_back(kBase58Alphabet[*it]);
  }
  return result;
}

// Fixed-width machine identifiers only: no trimming, prefixes or StrKey input.
// Reject oversized text before conversion and never allocate from input length.
template<std::size_t N>
std::optional<std::array<std::uint8_t, N>>
decode_base58_fixed(std::string_view text)
{
  if (text.size() > N * 138 / 100 + (N == 0 ? 0 : 1)) {
    return std::nullopt;
  }
  std::size_t zeros = 0;
  while (zeros < text.size() && text[zeros] == '1') {
    ++zeros;
  }
  if (zeros > N) {
    return std::nullopt;
  }
  std::array<std::uint8_t, N> bytes{};
  std::size_t length = 0;
  for (char c : text.substr(zeros)) {
    const auto digit = kBase58Alphabet.find(c);
    if (digit == std::string_view::npos) {
      return std::nullopt;
    }
    unsigned carry = static_cast<unsigned>(digit);
    for (std::size_t i = 0; i < length; ++i) {
      carry += static_cast<unsigned>(bytes[i]) * 58;
      bytes[i] = static_cast<std::uint8_t>(carry % 256);
      carry /= 256;
    }
    while (carry != 0) {
      if (length == N - zeros) {
        return std::nullopt;
      }
      bytes[length++] = static_cast<std::uint8_t>(carry % 256);
      carry /= 256;
    }
  }
  if (zeros + length != N) {
    return std::nullopt;
  }
  std::reverse(bytes.begin(), bytes.end());
  return bytes;
}

} // namespace hogd
