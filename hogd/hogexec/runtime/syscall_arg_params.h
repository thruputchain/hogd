#pragma once

#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/components/alias_guard.h"
#include "hogd/hogexec/runtime/syscall_argument_parsers.h"

namespace hogd {

// Validate and convert a uint64_t to a narrower integral type.
// Returns std::nullopt if the input is not a valid encoding of the target type:
// - Unsigned types narrower than uint64_t reject upper-bit overflow.
// - Signed types narrower than int64_t require sign-extended encoding.
// - 64-bit types accept all bit patterns.
template<std::integral T> requires (sizeof(T) <= 8)
constexpr std::optional<T>
narrow_int_checked(uint64_t input)
{
  if constexpr (std::is_signed_v<T>) {
    constexpr auto kNumBits = sizeof(T) * 8;

    if constexpr (kNumBits < 64) {
      constexpr uint64_t kLowBitsMask = (uint64_t{1} << kNumBits) - 1;
      constexpr uint64_t kUpperBitsMask = ~kLowBitsMask;
      constexpr uint64_t kSignBit = uint64_t{1} << (kNumBits - 1);

      auto upper_bits = input & kUpperBitsMask;
      bool sign_set = (input & kSignBit) != 0;

      if ((!sign_set && upper_bits != 0) ||
          (sign_set && upper_bits != kUpperBitsMask)) {
        return std::nullopt;
      }
    }

    using unsigned_t = std::make_unsigned_t<T>;
    auto narrowed = static_cast<unsigned_t>(input);
    return std::bit_cast<T>(narrowed);
  }
  else {
    if constexpr (sizeof(T) < sizeof(uint64_t)) {
      if (input > std::numeric_limits<T>::max()) {
        return std::nullopt;
      }
    }
    return static_cast<T>(input);
  }
}

// Templates detailing how syscall arguments are parsed from contract memory
// into typesafe objects to pass to inner syscalls.
//
// Each argument type consumes some number of uint64_t inputs to the syscall.
// These uint64_t inputs either encode the value (as in small types)
// or act as offsets into contract memory, or some combination thereof.
//
// Each argument goes through three phases.
// - parse(), which might be fallible (i.e. OOB mem access).
// - validate(), which checks whether parse succeeded
// - deref(), which turns the parsed value into something that's passed to the
// syscall.
//
// There also is the following method, used during native_exec.
// - parse_mock(), which is an analogue of parse().  Does not rely on an
// explicit contract_memory argument, but rather treats the inputs as regular
// pointers.
//
// This framework is separated into three methods for flexibility and
// convenience. This allows definition of syscall arguments that might be
// optionally supplied to be passed as std::optional<T> or T*, while
// non-optional args can be passed as T or T&.
template<typename T> struct SyscallArgParams;

template<> struct SyscallArgParams<bool> {
  constexpr static size_t kNumInputs = 1;

  constexpr static std::optional<bool> parse(AliasGuard & /*unused*/,
                                             std::span<std::byte> /*unused*/,
                                             uint64_t input)
  {
    if (input > 1) {
      return std::nullopt;
    }
    return static_cast<bool>(input);
  }

  constexpr static std::optional<bool> parse_mock(AliasGuard & /*unused*/,
                                                  uint64_t input)
  {
    if (input > 1) {
      return std::nullopt;
    }
    return static_cast<bool>(input);
  }
  constexpr static bool validate(std::optional<bool> value)
  {
    return value.has_value();
  }
  constexpr static bool deref(std::optional<bool> value)
  {
    assert(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
  }
};

// Integer types that fit in a single uint64 syscall argument.
// Delegates to narrow_int_checked for range validation.
template<std::integral T> requires (sizeof(T) <= 8 && !std::same_as<T, bool>)
struct SyscallArgParams<T> {
  constexpr static size_t kNumInputs = 1;

  constexpr static std::optional<T> parse(AliasGuard & /*unused*/,
                                          std::span<std::byte> /*unused*/,
                                          uint64_t input)
  {
    return narrow_int_checked<T>(input);
  }

  constexpr static std::optional<T> parse_mock(AliasGuard & /*unused*/,
                                               uint64_t input)
  {
    return narrow_int_checked<T>(input);
  }
  constexpr static bool validate(std::optional<T> value)
  {
    return value.has_value();
  }
  constexpr static T deref(std::optional<T> value)
  {
    assert(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
  }
};

// primitive type enum values
template<typename T> requires (std::is_enum_v<T> && sizeof(T) <= 8)
struct SyscallArgParams<T> {
  using underlying_t = std::underlying_type_t<T>;

  constexpr static size_t kNumInputs = 1;

  constexpr static std::optional<T> parse_impl(uint64_t input)
  {
    auto result = narrow_int_checked<underlying_t>(input);
    if (!result.has_value()) {
      return std::nullopt;
    }
    return static_cast<T>(*result);
  }

  constexpr static std::optional<T> parse(AliasGuard & /*unused*/,
                                          std::span<std::byte> /*unused*/,
                                          uint64_t input)
  {
    return parse_impl(input);
  }

  constexpr static std::optional<T> parse_mock(AliasGuard & /*unused*/,
                                               uint64_t input)
  {
    return parse_impl(input);
  }

  constexpr static bool validate(std::optional<T> value)
  {
    return value.has_value();
  }

  constexpr static T deref(std::optional<T> value)
  {
    assert(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
  }
};

// [u8] span contract output.
// The two input arguments are (offset, length) in contract memory.
template<> struct SyscallArgParams<std::span<std::byte>> {
  constexpr static size_t kNumInputs = 2;
  constexpr static std::optional<std::span<std::byte>>
  parse(AliasGuard &guard, std::span<std::byte> contract_memory,
        uint64_t offset, uint64_t len)
  {
    return check_user_span_argument(guard, contract_memory, offset, len);
  }

  constexpr static std::optional<std::span<std::byte>>
  parse_mock(AliasGuard &guard, uint64_t offset, uint64_t len)
  {
    guard.check_and_register(offset, len);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *output_pointer = reinterpret_cast<std::byte *>(offset);
    return std::span<std::byte>{output_pointer, len};
  }

  constexpr static bool validate(std::optional<std::span<std::byte>> arg)
  {
    return arg.has_value();
  }

  constexpr static std::span<std::byte>
  deref(std::optional<std::span<std::byte>> value)
  {
    assert(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
  }
};

// [u8] span contract input.
// The two input arguments are (offset, length) in contract memory.
template<> struct SyscallArgParams<std::span<const std::byte>> {
  constexpr static size_t kNumInputs = 2;

  constexpr static std::optional<std::span<const std::byte>>
  parse(AliasGuard &guard, std::span<std::byte> contract_memory,
        uint64_t offset, uint64_t len)
  {
    return check_user_span_argument(guard, contract_memory, offset, len);
  }

  constexpr static std::optional<std::span<const std::byte>>
  parse_mock(AliasGuard &guard, uint64_t offset, uint64_t len)
  {
    if (offset == 0 && len != 0) {
      // nullptr with non-zero length is invalid in native exec
      return std::nullopt;
    }
    guard.check_and_register(offset, len);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto *output_pointer = reinterpret_cast<const std::byte *>(offset);
    return std::span<const std::byte>{output_pointer, len};
  }

  constexpr static bool validate(std::optional<std::span<const std::byte>> arg)
  {
    return arg.has_value();
  }

  constexpr static std::span<const std::byte>
  deref(std::optional<std::span<const std::byte>> value)
  {
    assert(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
  }
};

// Trivially copyable input types.
// Passed as contract memory offsets (i.e. the single input argument).
template<typename T> requires std::is_trivially_copyable_v<T>
struct SyscallArgParams<const T &> {
  constexpr static size_t kNumInputs = 1;
  constexpr static std::optional<T> parse(AliasGuard &guard,
                                          std::span<std::byte> contract_memory,
                                          uint64_t offset)
  {
    return check_user_argument<const T>(guard, contract_memory, offset);
  }

  constexpr static std::optional<T> parse_mock(AliasGuard &guard,
                                               uint64_t offset)
  {
    return check_user_argument_mock<const T>(guard, offset);
  }

  constexpr static bool validate(std::optional<T> arg)
  {
    return arg.has_value();
  }

  constexpr static const T &deref(std::optional<T> const &value)
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
  }
};

// Trivially copyable output types..
// Passed as contract memory offsets (i.e. the single input argument).
template<typename T>
requires (std::is_trivially_copyable_v<T> && !std::is_const_v<T>)
struct SyscallArgParams<T &> {
  constexpr static size_t kNumInputs = 1;
  constexpr static WritableOutput<T> parse(AliasGuard &guard,
                                           std::span<std::byte> contract_memory,
                                           uint64_t offset)
  {
    return check_user_argument<T>(guard, contract_memory, offset);
  }

  constexpr static WritableOutput<T> parse_mock(AliasGuard &guard,
                                                uint64_t offset)
  {
    return check_user_argument_mock<T>(guard, offset);
  }

  constexpr static bool validate(WritableOutput<T> const &arg)
  {
    return arg.has_value();
  }

  constexpr static T &deref(WritableOutput<T> &value) { return value.get(); }
};

// Optional trivially copyable input types.
// Uses UINT64_MAX as an explicit null sentinel. Other offsets must resolve
// to in-bounds memory or argument validation fails.
// Some syscalls use nullptr to indicate an optional parameter is omitted.
template<typename T> requires std::is_trivially_copyable_v<T>
struct SyscallArgParams<const T *> {
  struct OptionalInputParseResult {
    bool is_valid = false;
    std::optional<T> value = std::nullopt;
  };

  constexpr static size_t kNumInputs = 1;
  constexpr static OptionalInputParseResult
  parse(AliasGuard &guard, std::span<std::byte> contract_memory,
        uint64_t offset)
  {
    // 0 can be a valid pointer in wasm. As such, optional inputs use
    // UINT64_MAX as the explicit null sentinel.
    if (offset == std::numeric_limits<uint64_t>::max()) {
      return {.is_valid = true, .value = std::nullopt};
    }

    auto value = check_user_argument<const T>(guard, contract_memory, offset);
    if (!value.has_value()) {
      return {.is_valid = false, .value = std::nullopt};
    }

    return {.is_valid = true, .value = std::move(value)};
  }

  constexpr static OptionalInputParseResult parse_mock(AliasGuard &guard,
                                                       uint64_t offset)
  {
    // 0 can be a valid pointer in wasm.  As such, the optional input types in
    // the sdk use UINT64_MAX as the nullptr (in the case of a None input),
    // instead of 0.
    if (offset == std::numeric_limits<uint64_t>::max()) {
      return {.is_valid = true, .value = std::nullopt};
    }

    return {.is_valid = true,
            .value = check_user_argument_mock<const T>(guard, offset)};
  }

  constexpr static bool validate(OptionalInputParseResult const &arg)
  {
    return arg.is_valid;
  }

  constexpr static const T *deref(OptionalInputParseResult const &value)
  {
    return value.value.has_value() ? &(*value.value) : nullptr;
  }
};

template<typename T>
concept is_syscall_arg_param = requires(T t) {
  { SyscallArgParams<T>::kNumInputs } -> std::convertible_to<size_t>;
};

} // namespace hogd
