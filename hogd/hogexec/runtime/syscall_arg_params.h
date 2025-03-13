#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/syscall_argument_parsers.h"

namespace hogd {

template<typename T> struct SyscallArgParams;

template<> struct SyscallArgParams<uint64_t> {
  constexpr static size_t kNumInputs = 1;

  constexpr static uint64_t parse(std::span<std::byte> /*unused*/,
                                  uint64_t input)
  {
    return input;
  }
  constexpr static bool validate(uint64_t /*unused*/) { return true; }
  constexpr static uint64_t deref(uint64_t value) { return value; }
};

template<> struct SyscallArgParams<std::span<std::byte>> {
  constexpr static size_t kNumInputs = 2;
  constexpr static std::optional<std::span<std::byte>>
  parse(std::span<std::byte> contract_memory, uint64_t offset, uint64_t len)
  {
    return check_user_span_argument(contract_memory, offset, len);
  }

  constexpr static bool validate(std::optional<std::span<std::byte>> arg)
  {
    return arg.has_value();
  }

  constexpr static std::span<std::byte>
  deref(std::optional<std::span<std::byte>> value)
  {
    return *value;
  }
};

template<> struct SyscallArgParams<std::span<const std::byte>> {
  constexpr static size_t kNumInputs = 2;

  constexpr static std::optional<std::span<const std::byte>>
  parse(std::span<std::byte> contract_memory, uint64_t offset, uint64_t len)
  {
    return check_user_span_argument(contract_memory, offset, len);
  }

  constexpr static bool validate(std::optional<std::span<const std::byte>> arg)
  {
    return arg.has_value();
  }

  constexpr static std::span<const std::byte>
  deref(std::optional<std::span<const std::byte>> value)
  {
    return *value;
  }
};

template<typename T> requires std::is_trivially_copyable_v<T> && (sizeof(T) > 8)
struct SyscallArgParams<T &> {
  constexpr static size_t kNumInputs = 1;
  constexpr static T *parse(std::span<std::byte> contract_memory,
                            uint64_t offset)
  {
    return check_user_argument<T>(contract_memory, offset, sizeof(T));
  }

  constexpr static bool validate(T *arg) { return (arg != nullptr); }

  constexpr static T &deref(T *value) { return *value; }
};

} // namespace hogd
