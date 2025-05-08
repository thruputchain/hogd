#pragma once

#include <concepts>
#include <cstddef>
#include <cstring>
#include <optional>
#include <print>
#include <span>
#include <type_traits>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "types.hh"

// Provides a set of tools to check that arguments provided by the
// runtime are well-defined, and contained within
// the runtime's active memory.

namespace hogd {

template<typename T> class WritableOutput {
public:
  WritableOutput(void *ptr) : value_(T{}), original_pointer_(ptr) {}

  WritableOutput() : value_(std::nullopt), original_pointer_(nullptr) {}

  T &get()
  {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return value_.value();
  }

  bool has_value() const { return value_.has_value(); }

  ~WritableOutput()
  {
    if (!value_)
      return;

    if constexpr (requires { value_->data(); }) {
      std::memcpy(original_pointer_, value_->data(), sizeof(T));
    }
    else {
      std::memcpy(original_pointer_, &(value_.value()), sizeof(T));
    }
  }

private:
  std::optional<T> value_;
  void *original_pointer_;
};

// check [base, base + len) \subseteq [0, memory_len)
inline bool
check_memory_range(uint64_t memory_len, uint64_t base, uint64_t len)
{
  uint64_t base_end = 0;
  if (integer_add_overflow(base, len, &base_end))
    return false;
  return (base_end <= memory_len);
}

template<typename T>
T
read_unaligned(const void *ptr)
{
  T value{};
  if constexpr (requires { value.data(); }) {
    std::memcpy(value.data(), ptr, sizeof(T));
  }
  else {
    std::memcpy(&value, ptr, sizeof(T));
  }
  std::println("read_unaligned: size {}", sizeof(T));
  return value;
}

template<typename T>
requires (std::is_trivially_copyable_v<T> && !std::is_const_v<T>)
WritableOutput<T>
check_user_argument(std::span<std::byte> contract_memory, uint64_t user_offset)
{
  if (!check_memory_range(contract_memory.size(), user_offset, sizeof(T))) {
    return WritableOutput<T>();
  }
  return WritableOutput<T>(contract_memory.data() + user_offset);
}

template<typename T>
requires (std::is_trivially_copyable_v<T> && !std::is_const_v<T>)
WritableOutput<T>
check_user_argument_mock(uint64_t user_offset)
{
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  auto *ptr = reinterpret_cast<void *>(user_offset);
  return WritableOutput<T>(ptr);
}

template<typename T>
requires (std::is_trivially_copyable_v<T> && std::is_const_v<T>)
std::optional<T>
check_user_argument(std::span<const std::byte> contract_memory,
                    uint64_t user_offset)
{
  if (!check_memory_range(contract_memory.size(), user_offset, sizeof(T))) {
    return std::nullopt;
  }
  return read_unaligned<std::remove_cvref_t<T>>(contract_memory.data() +
                                                user_offset);
}

template<typename T>
requires (std::is_trivially_copyable_v<T> && std::is_const_v<T>)
std::optional<T>
check_user_argument_mock(uint64_t user_offset)
{
  // NOLINTBEGIN(performance-no-int-to-ptr)
  return read_unaligned<std::remove_cvref_t<T>>(
      reinterpret_cast<void *>(user_offset));
  // NOLINTEND(performance-no-int-to-ptr)
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
// required type (i.e. the primitive_type in the syscall spec).. Infallible,
// but the requires clause checks at least that we did not misconfigure our
// syscall definitions.
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

// custom parsing in mock syscall inputs
template<typename T, typename... Args>
requires (std::same_as<Args, uint64_t> && ...)
std::optional<const T> check_user_custom_input_argument_mock(Args... args);

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
    if (auto ed25519 =
            check_user_argument<const Ed25519Signature>(contract_memory,
                                                        user_offset)) {
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
inline std::optional<const Signature>
check_user_custom_input_argument_mock(uint64_t user_discriminant,
                                      uint64_t user_offset)
{
  auto disc =
      check_user_primitive_argument<CryptographicScheme>(user_discriminant);

  Signature out;
  out.scheme(disc);
  switch (disc) {
  case CryptographicScheme::Ed25519:
  {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    out.ed25519() = *reinterpret_cast<const Ed25519Signature *>(user_offset);
    return out;
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
    if (auto ed25519 =
            check_user_argument<const Ed25519PublicKey>(contract_memory,
                                                        user_offset)) {
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
check_user_custom_input_argument_mock(uint64_t user_discriminant,
                                      uint64_t user_offset)
{
  auto disc =
      check_user_primitive_argument<CryptographicScheme>(user_discriminant);

  PublicKey out;
  out.scheme(disc);
  switch (disc) {
  case CryptographicScheme::Ed25519:
  {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    out.ed25519() = *reinterpret_cast<const Ed25519PublicKey *>(user_offset);
    return out;
  }
  default:
    return std::nullopt;
  }
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

} // namespace hogd
