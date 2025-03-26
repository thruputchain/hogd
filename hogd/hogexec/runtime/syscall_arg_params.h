#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
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
// This framework is separated into three methods for flexibility and
// convenience. This allows definition of syscall arguments that might be
// optionally supplied to be passed as std::optional<T> or T*, while
// non-optional args can be passed as T or T&.
template<typename T> struct SyscallArgParams;

// Primitive type syscall input
template<std::integral T> requires (sizeof(T) <= 8)
struct SyscallArgParams<T> {
  constexpr static size_t kNumInputs = 1;

  constexpr static T parse(std::span<std::byte> /*unused*/, uint64_t input)
  {
    return input;
  }
  constexpr static bool validate(uint64_t /*unused*/) { return true; }
  constexpr static T deref(T value) { return value; }
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
    assert(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
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
    assert(value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value;
  }
};

// Trivially copyable input and output types.
// Must be passed as contract memory offsets.
template<typename T> requires std::is_trivially_copyable_v<T>
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

// Optional trivially copyable input types.
template<typename T> requires std::is_trivially_copyable_v<T>
struct SyscallArgParams<const T *> {
  constexpr static size_t kNumInputs = 1;
  constexpr static T *parse(std::span<std::byte> contract_memory,
                            uint64_t offset)
  {
    return check_user_argument<T>(contract_memory, offset, sizeof(T));
  }

  constexpr static bool validate(T * /*unused*/) { return true; }

  constexpr static const T *deref(const T *value) { return value; }
};

// custom parse types
template<> struct SyscallArgParams<const Signature &> {
  constexpr static size_t kNumInputs = 2;
  constexpr static std::optional<const Signature>
  parse(std::span<std::byte> contract_memory, uint64_t arg1, uint64_t arg2)
  {
    return check_user_custom_input_argument<Signature>(contract_memory, arg1,
                                                       arg2);
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
