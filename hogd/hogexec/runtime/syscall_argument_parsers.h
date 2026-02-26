#pragma once

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"

// Provides a set of tools to check that arguments provided by the
// runtime are well-defined, and contained within
// the runtime's active memory.

namespace hogd {

template<typename T> class WritableOutput {
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
    assert(value_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return value_.value();
  }

  bool has_value() const noexcept { return value_.has_value(); }

  void disarm() noexcept
  {
    value_ = std::nullopt;
    original_pointer_ = nullptr;
  }

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

} // namespace hogd
