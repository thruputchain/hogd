#include "hogd/base/string.h"

#include <bit>
#include <cassert>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hogd/base/bits.h"
#include "hogd/base/cpputil.h"
#include "hogd/base/types.h"

namespace hogd {

namespace {

using Unit = std::pair<std::string, size_t>;

const Unit iec_byte_units[] = {
    {"PiB", 1UL << 50}, {"TiB", 1UL << 40}, {"GiB", 1UL << 30},
    {"MiB", 1UL << 20}, {"KiB", 1UL << 10}, {"B", 1},
};

const Unit si_byte_units[] = {
    {"PB", 1'000'000'000'000'000},
    {"TB", 1'000'000'000'000},
    {"GB", 1'000'000'000},
    {"MB", 1'000'000},
    {"KB", 1'000},
    {"B", 1},
};

template<size_t N>
std::string
pretty_print(const Unit (&tbl)[N], size_t val, bool add_space)
{
  for (const auto &[name, size] : tbl) {
    if (val < size)
      continue;
    return std::format("{:.4g}{}{}",
                       static_cast<double>(val) / static_cast<double>(size),
                       add_space ? " " : "", name);
  }

  return add_space ? "0 B" : "0B";
}

template<StringLiteral DictionaryStr>
std::string
encode_basen(std::span<const std::byte> buf)
{
  constexpr std::string_view kDictionary = DictionaryStr.get();
  static_assert(std::has_single_bit(kDictionary.size()));
  constexpr int kBits = std::countr_zero(kDictionary.size());
  constexpr uint32_t kMask = kDictionary.size() - 1;

  std::string out;
  out.reserve((buf.size() * kBitsPerByte + kBits - 1) / kBits);

  int bits_left = 0;
  uint32_t tmp = 0;
  for (std::byte byte : buf) {
    tmp = (tmp << kBitsPerByte) | static_cast<uint8_t>(byte);
    bits_left += kBitsPerByte;

    while (bits_left >= kBits) {
      out += kDictionary[(tmp >> (bits_left - kBits)) & kMask];
      bits_left -= kBits;
    }
  }

  if constexpr (kBitsPerByte % kBits != 0) {
    // handle any residual bits that don't divide evenly.
    assert(bits_left < kBits);
    if (bits_left > 0)
      out += kDictionary[(tmp << (kBits - bits_left)) & kMask];
  }

  return out;
}

bool
is_hex_digit(char c)
{
  return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
         (c >= '0' && c <= '9');
}

std::byte
hex_digit_to_nibble(char c)
{
  assert(is_hex_digit(c));
  if (c >= '0' && c <= '9')
    return static_cast<std::byte>(c - '0');
  if (c >= 'A' && c <= 'F')
    return static_cast<std::byte>(c - 'A' + 10);
  return static_cast<std::byte>(c - 'a' + 10);
}

} // namespace

std::string
print_iec_bytes(size_t val, bool add_space)
{
  return pretty_print(iec_byte_units, val, add_space);
}

std::string
print_si_bytes(size_t val, bool add_space)
{
  return pretty_print(si_byte_units, val, add_space);
}

std::string
encode_hex(std::span<const std::byte> buf)
{
  constexpr StringLiteral kHexDictionary("0123456789abcdef");
  static_assert(kHexDictionary.get().size() == 16);
  return encode_basen<kHexDictionary>(buf);
}

std::vector<std::byte>
decode_hex(std::string_view str)
{
  std::vector<std::byte> out;
  out.reserve(str.size() / 2);
  for (size_t i = 0; i < str.size(); i += 2) {
    std::byte byte =
        hex_digit_to_nibble(str[i]) << 4 | hex_digit_to_nibble(str[i + 1]);
    out.push_back(byte);
  }
  return out;
}

std::string
encode_base32(std::span<const std::byte> buf)
{
  constexpr StringLiteral kBase32Dictionary("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567");
  static_assert(kBase32Dictionary.get().size() == 32);
  return encode_basen<kBase32Dictionary>(buf);
}

std::string
encode_base64(std::span<const std::byte> buf)
{
  constexpr StringLiteral kBase64Dictionary(
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
  static_assert(kBase64Dictionary.get().size() == 64);
  return encode_basen<kBase64Dictionary>(buf);
}

std::string
encode_base64_url_safe(std::span<const std::byte> buf)
{
  constexpr StringLiteral kBase64Dictionary(
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
  static_assert(kBase64Dictionary.get().size() == 64);
  return encode_basen<kBase64Dictionary>(buf);
}

} // namespace hogd
