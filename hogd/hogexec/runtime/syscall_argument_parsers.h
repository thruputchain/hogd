#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

inline bool
check_memory_range(uint64_t memory_len, uint64_t base, uint64_t len)
{
  uint64_t base_end = 0;
  if (integer_add_overflow(base, len, &base_end))
    return false;
  return (base_end <= memory_len);
}

template<typename T> requires std::is_trivially_copyable_v<T>
T *
check_user_argument(std::span<std::byte> contract_memory, uint64_t user_offset,
                    uint64_t user_len)
{
  if (!check_memory_range(contract_memory.size(), user_offset, user_len)) {
    return nullptr;
  }
  return reinterpret_cast<T *>(contract_memory.data() + user_offset);
}

template<typename T> requires std::is_trivially_copyable_v<T>
const T *
check_user_argument(std::span<const std::byte> contract_memory,
                    uint64_t user_offset, uint64_t user_len)
{
  if (!check_memory_range(contract_memory.size(), user_offset, user_len)) {
    return nullptr;
  }
  return reinterpret_cast<const T *>(contract_memory.data() + user_offset);
}

inline std::optional<std::span<std::byte>>
check_user_span_argument(std::span<std::byte> contract_memory,
                         uint64_t user_offset, uint64_t user_len)
{
  if (!check_memory_range(contract_memory.size(), user_offset, user_len)) {
    return std::nullopt;
  }
  return contract_memory.subspan(user_offset, user_len);
}

template<typename T> requires (std::is_integral_v<T> || std::is_enum_v<T>)
T
check_user_primitive_argument(uint64_t user_value)
{
  return static_cast<T>(user_value);
}

// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
template<typename T, typename... Args>
requires (std::same_as<Args, uint64_t> && ...)
std::optional<const T>
check_user_custom_input_argument(std::span<const std::byte> contract_memory,
                                 Args... args);

template<>
inline std::optional<const Signature>
check_user_custom_input_argument(std::span<const std::byte> contract_memory,
                                 uint64_t user_discriminant,
                                 uint64_t user_offset)
{
  auto disc =
      check_user_primitive_argument<CryptographicScheme>(user_discriminant);
  Signature out;
  out.scheme(disc);
  switch (disc) {
  case CryptographicScheme::Ed25519:
  {
    if (auto *ed25519 =
            check_user_argument<Ed25519Signature>(contract_memory, user_offset,
                                                  sizeof(Ed25519Signature))) {
      out.ed25519() = *ed25519;
      return out;
    }
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

template<>
inline std::optional<const PublicKey>
check_user_custom_input_argument(std::span<const std::byte> contract_memory,
                                 uint64_t user_discriminant,
                                 uint64_t user_offset)
{
  auto disc =
      check_user_primitive_argument<CryptographicScheme>(user_discriminant);

  PublicKey out;
  out.scheme(disc);
  switch (disc) {
  case CryptographicScheme::Ed25519:
  {
    if (const auto *ed25519 =
            check_user_argument<Ed25519PublicKey>(contract_memory, user_offset,
                                                  sizeof(Ed25519Signature))) {
      out.ed25519() = *ed25519;
      return out;
    }
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

} // namespace hogd
