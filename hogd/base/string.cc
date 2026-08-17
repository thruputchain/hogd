#include "hogd/base/string.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hogd/base/bits.h"
#include "hogd/base/cpputil.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"

namespace hogd {

namespace {

struct Unit {
  std::string_view name;
  size_t size;
};

constexpr Unit kIecByteUnits[] = {
    {.name = "PiB", .size = 1UL << 50}, {.name = "TiB", .size = 1UL << 40},
    {.name = "GiB", .size = 1UL << 30}, {.name = "MiB", .size = 1UL << 20},
    {.name = "KiB", .size = 1UL << 10}, {.name = "B", .size = 1},
};

constexpr Unit kSiByteUnits[] = {
    {.name = "PB", .size = 1'000'000'000'000'000},
    {.name = "TB", .size = 1'000'000'000'000},
    {.name = "GB", .size = 1'000'000'000},
    {.name = "MB", .size = 1'000'000},
    {.name = "KB", .size = 1'000},
    {.name = "B", .size = 1},
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
  constexpr std::u8string_view kDictionary = DictionaryStr.get();
  static_assert(std::has_single_bit(kDictionary.size()));
  constexpr int kBits = std::countr_zero(kDictionary.size());
  constexpr uint32_t kMask = kDictionary.size() - 1;

  std::string out;
  out.reserve(((buf.size() * kBitsPerByte) + kBits - 1) / kBits);

  int bits_left = 0;
  uint32_t tmp = 0;
  for (std::byte byte : buf) {
    tmp = (tmp << kBitsPerByte) | static_cast<uint8_t>(byte);
    bits_left += kBitsPerByte;

    while (bits_left >= kBits) {
      out +=
          static_cast<char>(kDictionary[(tmp >> (bits_left - kBits)) & kMask]);
      bits_left -= kBits;
    }
  }

  if constexpr (kBitsPerByte % kBits != 0) {
    // handle any residual bits that don't divide evenly.
    assert(bits_left < kBits);
    if (bits_left > 0)
      out +=
          static_cast<char>(kDictionary[(tmp << (kBits - bits_left)) & kMask]);
  }

  return out;
}

[[maybe_unused]] bool
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
  return pretty_print(kIecByteUnits, val, add_space);
}

std::string
print_si_bytes(size_t val, bool add_space)
{
  return pretty_print(kSiByteUnits, val, add_space);
}

std::string
print_iec_rate(size_t bytes, double seconds, bool add_space)
{
  // A zero interval is permitted by steady_clock, so the quotient can be
  // infinity (bytes > 0) or NaN (bytes == 0). Screen both out, along with a
  // magnitude that would not fit, before converting to size_t: every one of
  // those conversions is undefined behavior (#2900).
  double rate = static_cast<double>(bytes) / seconds;
  if (!(seconds > 0) || !std::isfinite(rate))
    return print_iec_bytes(0, add_space);

  // static_cast<double>(SIZE_MAX) rounds up to exactly 2^64, so this rejects
  // precisely the values that would overflow the conversion.
  constexpr double kLimit =
      static_cast<double>(std::numeric_limits<size_t>::max());
  if (rate >= kLimit)
    return print_iec_bytes(std::numeric_limits<size_t>::max(), add_space);

  return print_iec_bytes(static_cast<size_t>(rate), add_space);
}

std::string
encode_hex(std::span<const std::byte> buf)
{
  constexpr StringLiteral kHexDictionary(u8"0123456789abcdef");
  static_assert(kHexDictionary.get().size() == 16);
  return encode_basen<kHexDictionary>(buf);
}

std::vector<std::byte>
decode_hex(std::string_view str)
{
  CHECK(str.size() % 2 == 0);
  std::vector<std::byte> out;
  out.reserve(str.size() / 2);
  for (size_t i = 0; i + 1 < str.size(); i += 2) {
    std::byte byte =
        hex_digit_to_nibble(str[i]) << 4 | hex_digit_to_nibble(str[i + 1]);
    out.push_back(byte);
  }
  return out;
}

std::string
encode_base32(std::span<const std::byte> buf)
{
  constexpr StringLiteral kBase32Dictionary(
      u8"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567");
  static_assert(kBase32Dictionary.get().size() == 32);
  return encode_basen<kBase32Dictionary>(buf);
}

std::string
encode_base64(std::span<const std::byte> buf)
{
  constexpr StringLiteral kBase64Dictionary(
      u8"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
  static_assert(kBase64Dictionary.get().size() == 64);
  return encode_basen<kBase64Dictionary>(buf);
}

std::string
encode_base64_url_safe(std::span<const std::byte> buf)
{
  constexpr StringLiteral kBase64Dictionary(
      u8"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
  static_assert(kBase64Dictionary.get().size() == 64);
  return encode_basen<kBase64Dictionary>(buf);
}

} // namespace hogd
