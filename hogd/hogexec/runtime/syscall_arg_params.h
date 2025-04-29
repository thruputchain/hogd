#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/syscall_argument_parsers.h"
#include "types.hh"

namespace hogd {

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

// Primitive type syscall input
// For items that can be passed directly within one of the uint64 syscall
// arguments, and not as a memory offset.
template<std::integral T> requires (sizeof(T) <= 8)
struct SyscallArgParams<T> {
  constexpr static size_t kNumInputs = 1;

  constexpr static T parse(std::span<std::byte> /*unused*/, uint64_t input)
  {
    return input;
  }

  constexpr static T parse_mock(uint64_t input) { return input; }
  constexpr static bool validate(T /*unused*/) { return true; }
  constexpr static T deref(T value) { return value; }
};

// Input types that can be passed by value but not within a uint64
template<typename T> requires (sizeof(T) > 8) && std::is_trivially_copyable_v<T>
struct SyscallArgParams<T> {
  constexpr static size_t kNumInputs = 1;

  constexpr static std::optional<const T>
  parse(std::span<std::byte> contract_memory, uint64_t offset)
  {
    return check_user_argument<const T>(contract_memory, offset);
  }

  constexpr static std::optional<const T> parse_mock(uint64_t offset)
  {
    return check_user_argument_mock<const T>(offset);
  }

  constexpr static bool validate(std::optional<const T> argument)
  {
    return argument.has_value();
  }
  constexpr static T deref(std::optional<const T> &value)
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
  }
};

// [u8] span contract output.
// The two input arguments are (offset, length) in contract memory.
template<> struct SyscallArgParams<std::span<std::byte>> {
  constexpr static size_t kNumInputs = 2;
  constexpr static std::optional<std::span<std::byte>>
  parse(std::span<std::byte> contract_memory, uint64_t offset, uint64_t len)
  {
    return check_user_span_argument(contract_memory, offset, len);
  }

  constexpr static std::optional<std::span<std::byte>>
  parse_mock(uint64_t offset, uint64_t len)
  {
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
  parse(std::span<std::byte> contract_memory, uint64_t offset, uint64_t len)
  {
    return check_user_span_argument(contract_memory, offset, len);
  }

  constexpr static std::optional<std::span<const std::byte>>
  parse_mock(uint64_t offset, uint64_t len)
  {
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
  constexpr static std::optional<T> parse(std::span<std::byte> contract_memory,
                                          uint64_t offset)
  {
    return check_user_argument<const T>(contract_memory, offset);
  }

  constexpr static std::optional<T> parse_mock(uint64_t offset)
  {
    return check_user_argument_mock<const T>(offset);
  }

  constexpr static bool validate(std::optional<T> arg)
  {
    return arg.has_value();
  }

  constexpr static T &deref(std::optional<T> &value)
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
  constexpr static WritableOutput<T> parse(std::span<std::byte> contract_memory,
                                           uint64_t offset)
  {
    return check_user_argument<T>(contract_memory, offset);
  }

  constexpr static WritableOutput<T> parse_mock(uint64_t offset)
  {
    return check_user_argument_mock<T>(offset);
  }

  constexpr static bool validate(WritableOutput<T> const &arg)
  {
    return arg.has_value();
  }

  constexpr static T &deref(WritableOutput<T> &value) { return value.get(); }
};

// Optional trivially copyable input types.
// The difference between this and the non-optional case
// is that validation always succeeds, but the syscall itself
// might be passed a nullptr (and must check against that).
// Some syscalls use that optional check to provide default
// parameters if a parameter is not supplied by a contract.
template<typename T> requires std::is_trivially_copyable_v<T>
struct SyscallArgParams<const T *> {
  constexpr static size_t kNumInputs = 1;
  constexpr static std::optional<T> parse(std::span<std::byte> contract_memory,
                                          uint64_t offset)
  {
    return check_user_argument<const T>(contract_memory, offset);
  }

  constexpr static std::optional<T> parse_mock(uint64_t offset)
  {
    // 0 can be a valid pointer in wasm.  As such, the optional input types in
    // the sdk use UINT64_MAX as the nullptr (in the case of a None input),
    // instead of 0.
    if (offset == std::numeric_limits<uint64_t>::max()) {
      return std::nullopt;
    }
    return check_user_argument_mock<const T>(offset);
  }

  constexpr static bool validate(std::optional<T> /*unused*/) { return true; }

  constexpr static const T *deref(std::optional<T> &value)
  {
    return value.has_value() ? &(*value) : nullptr;
  }
};

// custom parse input types.
// For those types that don't quite fit into any of the above categories.
// At this point, these include PublicKeys and Signatures,
// which are both unions over different cryptographic schemes.
// The first argument is an enum of the cryptographic scheme,
// and the second can be scheme-dependent, but refers to whatever
// data is required for the scheme.
// (Future schemes might require variable numbers of arguments; that is not
// implemented here).)
template<> struct SyscallArgParams<const Signature &> {
  constexpr static size_t kNumInputs = 2;
  constexpr static std::optional<const Signature>
  parse(std::span<std::byte> contract_memory, uint64_t arg1, uint64_t arg2)
  {
    return check_user_custom_input_argument<Signature>(contract_memory, arg1,
                                                       arg2);
  }

  constexpr static std::optional<const Signature> parse_mock(uint64_t arg1,
                                                             uint64_t arg2)
  {
    return check_user_custom_input_argument_mock<Signature>(arg1, arg2);
  }

  constexpr static bool validate(std::optional<const Signature> const &arg)
  {
    return arg.has_value();
  }

  constexpr static const Signature &
  deref(std::optional<const Signature> const &value)
  {
    assert(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
  }
};

template<> struct SyscallArgParams<const PublicKey &> {
  constexpr static size_t kNumInputs = 2;
  constexpr static std::optional<const PublicKey>
  parse(std::span<std::byte> contract_memory, uint64_t arg1, uint64_t arg2)
  {
    return check_user_custom_input_argument<PublicKey>(contract_memory, arg1,
                                                       arg2);
  }

  constexpr static std::optional<const PublicKey> parse_mock(uint64_t arg1,
                                                             uint64_t arg2)
  {
    return check_user_custom_input_argument_mock<PublicKey>(arg1, arg2);
  }

  constexpr static bool validate(std::optional<const PublicKey> const &arg)
  {
    return arg.has_value();
  }

  constexpr static const PublicKey &
  deref(std::optional<const PublicKey> const &value)
  {
    assert(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
  }
};

} // namespace hogd
