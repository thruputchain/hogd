#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>

#include "hogd/base/compiler.h"
#include "types.hh"

// Provides a set of tools to check that arguments provided by the
// runtime are well-defined, and contained within
// the runtime's active memory.

namespace hogd {

// check [base, base + len) \subseteq [0, memory_len)
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

inline std::optional<std::span<std::byte>>
check_user_span_argument(std::span<std::byte> contract_memory,
                         uint64_t user_offset, uint64_t user_len)
{
  if (!check_memory_range(contract_memory.size(), user_offset, user_len)) {
    return std::nullopt;
  }
  return contract_memory.subspan(user_offset, user_len);
}

// Convert arguments passed as parameters to the wasm host function to the
// required type. Infallible, but the requires clause checks at least that we
// did not misconfigure our syscall definitions.
template<typename T> requires (std::is_integral_v<T> || std::is_enum_v<T>)
T
check_user_primitive_argument(uint64_t user_value)
{
  return static_cast<T>(user_value);
}

} // namespace hogd
