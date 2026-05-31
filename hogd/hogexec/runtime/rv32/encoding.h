#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace hogd::rv32 {

inline constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
inline constexpr uint64_t kFnvPrime = 1099511628211ULL;

constexpr uint32_t
load_u32_le(std::span<const uint8_t> bytes, size_t offset)
{
  return uint32_t{bytes[offset]} | (uint32_t{bytes[offset + 1]} << 8) |
         (uint32_t{bytes[offset + 2]} << 16) |
         (uint32_t{bytes[offset + 3]} << 24);
}

constexpr uint64_t
load_u64_le(std::span<const uint8_t> bytes, size_t offset)
{
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    value |= uint64_t{bytes[offset + i]} << (i * 8);
  }
  return value;
}

constexpr void
store_u32_le(std::span<uint8_t> bytes, size_t offset, uint32_t value)
{
  for (size_t i = 0; i < sizeof(uint32_t); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

constexpr void
store_u64_le(std::span<uint8_t> bytes, size_t offset, uint64_t value)
{
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

constexpr uint64_t
fnv_digest_byte(uint64_t digest, uint8_t value)
{
  return (digest ^ value) * kFnvPrime;
}

constexpr uint64_t
fnv_digest_u64(uint64_t digest, uint64_t value)
{
  for (int i = 0; i < 8; ++i) {
    digest = fnv_digest_byte(digest, static_cast<uint8_t>(value));
    value >>= 8;
  }
  return digest;
}

constexpr uint64_t
fnv_digest_name(uint64_t digest, const char *name)
{
  for (const char *cursor = name; *cursor != '\0'; ++cursor) {
    digest = fnv_digest_byte(digest, static_cast<uint8_t>(*cursor));
  }
  return fnv_digest_byte(digest, 0);
}

} // namespace hogd::rv32
