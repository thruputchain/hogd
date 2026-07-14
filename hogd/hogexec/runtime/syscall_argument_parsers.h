#pragma once

#include <bit>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/components/alias_guard.h"

// Provides a set of tools to check that arguments provided by the
// runtime are well-defined, and contained within
// the runtime's active memory.

namespace hogd {

// These parsers will need to be rewritten to support big-endian machines.
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(bool) == 1);

template<typename T> class WritableOutput {
  // The destructor copies all sizeof(T) bytes of the staged value into contract
  // memory, so no byte of T may be indeterminate. Implicit padding bytes are:
  // they hold whatever the host stack last left there. Writing them back leaks
  // host memory to an untrusted contract, and makes the bytes the contract
  // observes depend on the compiler rather than on the value, so two honest
  // validators could hand the same contract different bytes.
  //
  // If this fires, T has padding. Give the gaps names -- declare them as
  // `reserved` fields, so that initializing T zeroes them like any other field
  // -- rather than scrubbing T at the call site, which only holds until the
  // next person adds a variant. See generated::AuthClaim in ffi_types.h.
  static_assert(std::has_unique_object_representations_v<T>,
                "syscall output types must not have padding bytes: name the "
                "gaps as reserved fields (see ffi_types.h)");

public:
  WritableOutput(void *ptr) : value_(T{}), original_pointer_(ptr) {}

  WritableOutput() : value_(std::nullopt), original_pointer_(nullptr) {}

  WritableOutput(WritableOutput &&other) noexcept
    : value_(std::move(other.value_)),
      original_pointer_(other.original_pointer_)
  {
    other.value_ = std::nullopt;
    other.original_pointer_ = nullptr;
  }
  WritableOutput(const WritableOutput &) = delete;
  WritableOutput &operator=(const WritableOutput &) = delete;
  WritableOutput &operator=(WritableOutput &&other) = delete;

  T &get()
  {
    CHECK(value_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *value_;
  }

  bool has_value() const noexcept { return value_.has_value(); }

  const void *target_ptr() const noexcept { return original_pointer_; }
  static constexpr size_t kWriteSize = sizeof(T);

  void disarm() noexcept
  {
    value_ = std::nullopt;
    original_pointer_ = nullptr;
  }

  ~WritableOutput()
  {
    if (!value_.has_value())
      return;

    if constexpr (std::is_same_v<T, bool>) {
      uint8_t canonical = (*value_) ? 1 : 0;
      std::memcpy(original_pointer_, &canonical, 1);
    }
    else if constexpr (requires { value_->data(); }) {
      std::memcpy(original_pointer_, value_->data(), sizeof(T));
    }
    else {
      std::memcpy(original_pointer_, &(*value_), sizeof(T));
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
  return value;
}

template<typename T>
requires (std::is_trivially_copyable_v<T> && !std::is_const_v<T>)
WritableOutput<T>
check_user_argument(AliasGuard &guard, std::span<std::byte> contract_memory,
                    uint64_t user_offset)
{
  guard.check_and_register(user_offset, sizeof(T));
  if (!check_memory_range(contract_memory.size(), user_offset, sizeof(T))) {
    return WritableOutput<T>();
  }
  return WritableOutput<T>(contract_memory.data() + user_offset);
}

template<typename T>
requires (std::is_trivially_copyable_v<T> && !std::is_const_v<T>)
WritableOutput<T>
check_user_argument_mock(AliasGuard &guard, uint64_t user_offset)
{
  if (user_offset == 0) {
    // nullptr
    return WritableOutput<T>();
  }
  guard.check_and_register(user_offset, sizeof(T));
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  auto *ptr = reinterpret_cast<void *>(user_offset);
  return WritableOutput<T>(ptr);
}

template<typename T>
requires (std::is_trivially_copyable_v<T> && std::is_const_v<T>)
std::optional<T>
check_user_argument(AliasGuard &guard,
                    std::span<const std::byte> contract_memory,
                    uint64_t user_offset)
{
  guard.check_and_register(user_offset, sizeof(T));
  if (!check_memory_range(contract_memory.size(), user_offset, sizeof(T))) {
    return std::nullopt;
  }
  return read_unaligned<std::remove_cvref_t<T>>(contract_memory.data() +
                                                user_offset);
}

template<typename T>
requires (std::is_trivially_copyable_v<T> && std::is_const_v<T>)
std::optional<T>
check_user_argument_mock(AliasGuard &guard, uint64_t user_offset)
{
  if (user_offset == 0) {
    // nullptr
    std::optional<T> result = std::nullopt;
    return result;
  }
  guard.check_and_register(user_offset, sizeof(T));
  // NOLINTBEGIN(performance-no-int-to-ptr)
  return read_unaligned<std::remove_cvref_t<T>>(
      reinterpret_cast<void *>(user_offset));
  // NOLINTEND(performance-no-int-to-ptr)
}

inline std::optional<std::span<std::byte>>
check_user_span_argument(AliasGuard &guard,
                         std::span<std::byte> contract_memory,
                         uint64_t user_offset, uint64_t user_len)
{
  guard.check_and_register(user_offset, user_len);
  if (!check_memory_range(contract_memory.size(), user_offset, user_len)) {
    return std::nullopt;
  }
  return contract_memory.subspan(user_offset, user_len);
}

} // namespace hogd
