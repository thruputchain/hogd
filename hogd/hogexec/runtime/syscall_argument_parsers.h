#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

inline bool
check_memory_span(MemorySpan span, uint64_t base, uint64_t len, bool write)
{
  if (write && !span.writable_) {
    return false;
  }
  // assert [base, base+len) \subset [span.base_offset_, span.base_offset_ + span.bytes_.size())
  if (base < span.base_offset_) {
    return false;
  }

  uint64_t base_end = 0;
  if (integer_add_overflow(base - span.base_offset_, len, &base_end))
    return false;
  return (base_end <= span.bytes_.size());
}

/*
inline bool check_memory(Memory& mem, uint64_t base, uint64_t len, bool write) {
  return [&]<std::size_t... I> (std::index_sequence<I...>) -> bool {
    return (check_memory_span(mem.spans_[I], base, len, write) || ...);
  }(std::make_index_sequence<Memory::kMaxSpans>());
}*/

template<typename T, bool WRITE> requires std::is_trivially_copyable_v<T>
T *
check_user_argument(MemorySpan span, uint64_t user_offset,
                    uint64_t user_len)
{
  if (!check_memory_span(span, user_offset, user_len, WRITE))
  {
    return nullptr;
  }
  return reinterpret_cast<T *>(span.bytes_.data() - span.base_offset_ + user_offset);
}

template<bool WRITE>
std::optional<std::span<std::byte>>
check_user_span_argument(MemorySpan span, uint64_t user_offset,
                    uint64_t user_len)
{
  if (!check_memory_span(span, user_offset, user_len, WRITE))
  {
    return std::nullopt;
  }
  return span.bytes_.subspan(user_offset - span.base_offset_, user_len);
}

template<typename T> requires std::is_trivially_copyable_v<T>
T *
check_user_argument(Memory& contract_memory, uint64_t user_offset,
                    uint64_t user_len)
{
  constexpr static bool kWrite = std::is_const_v<T>;
  return [&]<std::size_t...I>(std::index_sequence<I...>) {
    T* out = nullptr;
    ((out = check_user_argument<T, kWrite>(contract_memory.spans_[I], user_offset, user_len)) || ...);
    return out;
  }(std::make_index_sequence<Memory::kMaxSpans>());
}


template<bool WRITE>
std::optional<std::span<std::conditional_t<WRITE, std::byte, const std::byte>>>
check_user_span_argument(Memory& contract_memory,
                         uint64_t user_offset, uint64_t user_len)
{
  return [&]<std::size_t...I>(std::index_sequence<I...>) {
    std::optional<std::span<std::conditional_t<WRITE, std::byte, const std::byte>>> out;
    ((out = check_user_span_argument<WRITE>(contract_memory.spans_[I], user_offset, user_len)) || ...);
    return out;
  }(std::make_index_sequence<Memory::kMaxSpans>());
}

template<typename T> requires (std::is_integral_v<T> || std::is_enum_v<T>)
T
check_user_primitive_argument(uint64_t user_value)
{
  return static_cast<T>(user_value);
}

/*

template<typename T, typename... Args>
requires (std::same_as<Args, uint64_t> && ...)
std::optional<const T>
check_user_custom_input_argument(std::span<std::byte> contract_memory,
                                 Args... args);

template<>
inline std::optional<const Signature>
check_user_custom_input_argument(std::span<std::byte> contract_memory,
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
check_user_custom_input_argument(std::span<std::byte> contract_memory,
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
    if (auto *ed25519 =
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
} */

} // namespace hogd
