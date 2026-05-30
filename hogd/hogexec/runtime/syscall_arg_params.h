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

// Validate and convert a 32-bit syscall argument slot into a narrower-or-equal
// integral type T. This is the canonical wasm32 ABI encoding: each value slot
// is a bare 32-bit value (i32 on the wasm boundary), NOT widened or
// sign-extended to 64 bits.
//
// Returns std::nullopt if the bits are not a valid encoding of the target type:
// - Unsigned types narrower than uint32_t reject upper-bit overflow.
// - Signed types narrower than int32_t require sign-extended encoding.
// - 32-bit types accept all bit patterns.
//
// By-value syscall arguments occupy a single 32-bit slot, so T must be at most
// 32 bits wide; wider types (e.g. a 64-bit BlockHeight) must be passed by
// reference (as an offset into contract memory). This is enforced below.
template<std::integral T> requires (!std::same_as<T, bool>)
constexpr std::optional<T>
narrow_slot(uint32_t input)
{
  static_assert(sizeof(T) <= 4,
                "by-value syscall arguments occupy a single 32-bit slot; pass "
                "wider types by reference (as a memory offset) instead");

  if constexpr (std::is_signed_v<T>) {
    constexpr auto kNumBits = sizeof(T) * 8;

    if constexpr (kNumBits < 32) {
      constexpr uint32_t kLowBitsMask = (uint32_t{1} << kNumBits) - 1;
      constexpr uint32_t kUpperBitsMask = ~kLowBitsMask;
      constexpr uint32_t kSignBit = uint32_t{1} << (kNumBits - 1);

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
    if constexpr (sizeof(T) < sizeof(uint32_t)) {
      if (input > std::numeric_limits<T>::max()) {
        return std::nullopt;
      }
    }
    return static_cast<T>(input);
  }
}

// Mock/native (test_ffi) analogue of narrow_slot. The mock entrypoint keeps
// u64 argument slots so it can carry host pointers in offset slots; it rejects
// a *value* slot that does not fit a 32-bit slot up front as an FFI usage error
// (see mock_syscall_parser), so by the time we get here the value fits in 32
// bits. The defensive `> UINT32_MAX` guard is a belt-and-suspenders fallback
// that returns std::nullopt rather than silently truncating.
template<std::integral T> requires (!std::same_as<T, bool>)
constexpr std::optional<T>
narrow_slot_mock(uint64_t input)
{
  if (input > std::numeric_limits<uint32_t>::max()) {
    return std::nullopt;
  }
  return narrow_slot<T>(static_cast<uint32_t>(input));
}

// Templates detailing how syscall arguments are parsed from contract memory
// into typesafe objects to pass to inner syscalls.
//
// Each argument type consumes some number of argument slots to the syscall.
// On the wasm32 path each slot is a uint32_t (parse()); on the native mock
// path each slot is a uint64_t (parse_mock()), so the mock can carry host
// pointers in offset slots. These slots either encode the value (as in small
// types) or act as offsets into contract memory, or some combination thereof.
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
                                             uint32_t input)
  {
    if (input > 1) {
      return std::nullopt;
    }
    return static_cast<bool>(input);
  }

  // Mock keeps a u64 slot; the > 1 check already rejects any value (32-bit or
  // wider) that is not a valid bool encoding, so no separate 32-bit gate is
  // needed here.
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

// Integer types that fit in a single 32-bit syscall argument slot.
// Delegates to narrow_slot for range validation. narrow_slot static_asserts
// that T is at most 32 bits wide (wider types must be passed by reference).
template<std::integral T> requires (sizeof(T) <= 8 && !std::same_as<T, bool>)
struct SyscallArgParams<T> {
  constexpr static size_t kNumInputs = 1;

  constexpr static std::optional<T> parse(AliasGuard & /*unused*/,
                                          std::span<std::byte> /*unused*/,
                                          uint32_t input)
  {
    return narrow_slot<T>(input);
  }

  constexpr static std::optional<T> parse_mock(AliasGuard & /*unused*/,
                                               uint64_t input)
  {
    return narrow_slot_mock<T>(input);
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

  constexpr static std::optional<T>
  from_underlying(std::optional<underlying_t> result)
  {
    if (!result.has_value()) {
      return std::nullopt;
    }
    return static_cast<T>(*result);
  }

  constexpr static std::optional<T> parse(AliasGuard & /*unused*/,
                                          std::span<std::byte> /*unused*/,
                                          uint32_t input)
  {
    return from_underlying(narrow_slot<underlying_t>(input));
  }

  constexpr static std::optional<T> parse_mock(AliasGuard & /*unused*/,
                                               uint64_t input)
  {
    return from_underlying(narrow_slot_mock<underlying_t>(input));
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
        uint32_t offset, uint32_t len)
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
        uint32_t offset, uint32_t len)
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
                                          uint32_t offset)
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
                                           uint32_t offset)
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
// Uses an all-ones offset as an explicit null sentinel: UINT32_MAX on the
// wasm32 path (a 32-bit slot) and UINT64_MAX on the native mock path. Other
// offsets must resolve to in-bounds memory or argument validation fails.
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
        uint32_t offset)
  {
    // 0 can be a valid pointer in wasm. As such, optional inputs use
    // UINT32_MAX as the explicit null sentinel.
    if (offset == std::numeric_limits<uint32_t>::max()) {
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
