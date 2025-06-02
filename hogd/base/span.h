// span.h - additional features for spans

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "hogd/base/unique_function.h"

// enable hashing of const byte spans.
template<> struct std::hash<std::span<const std::byte>> {
  std::size_t operator()(const std::span<const std::byte> &span) const noexcept
  {
    return std::hash<std::string_view>{}(
        {reinterpret_cast<const char *>(span.data()), span.size()});
  }
};

namespace hogd {

// Cast an object as a const byte span.
template<typename T> requires std::is_trivially_copyable_v<T>
std::span<const std::byte, sizeof(T)>
byte_view(const T &t)
{
  return std::as_bytes(std::span<const T, 1>{std::addressof(t), 1});
}

// Cast an object as a mutable byte span.
template<typename T> requires std::is_trivially_copyable_v<T>
std::span<std::byte, sizeof(T)>
writable_byte_view(T &t)
{
  return std::as_writable_bytes(std::span<T, 1>{std::addressof(t), 1});
}

// Cast a legacy UNIX buffer to a byte span.
inline std::span<const std::byte>
byte_view(const char *buf, size_t len)
{
  return {reinterpret_cast<const std::byte *>(buf), len};
}

// Cast a legacy UNIX buffer to a mutable byte span.
inline std::span<std::byte>
writable_byte_view(char *buf, size_t len)
{
  return {reinterpret_cast<std::byte *>(buf), len};
}

// enable <=> comparisons between const byte spans.
constexpr std::strong_ordering
operator<=>(const std::span<const std::byte> lhs,
            const std::span<const std::byte> rhs)
{
  return std::lexicographical_compare_three_way(lhs.begin(), lhs.end(),
                                                rhs.begin(), rhs.end());
}

// enable == comparisons between const byte spans.
constexpr bool
operator==(const std::span<const std::byte> lhs,
           const std::span<const std::byte> rhs)
{
  return std::ranges::equal(lhs, rhs);
}

// OwningSpan adds ownership or a cleanup task to a std::span.
//
// It can be useful for situations where an object should be accessed through
// a std::span but the callee must take ownership of it or perform some other
// form of cleanup. For ownership, a std::vector could be used instead, but it
// would require data to be copied if it is stored in a different
// representation (e.g., a std::array or std::string), which is inefficient.
// For cleanup, an OwningSpan might be useful, for example, if you want to
// prevent an object from being freed in order to provide temporary zero-copy
// access to it.
template<typename T, std::size_t Extent = std::dynamic_extent>
class OwningSpan : public std::span<T, Extent> {
public:
  OwningSpan() = default;
  OwningSpan(std::span<T, Extent> span, UniqueFunction<void()> &&cleanup_fn)
    : std::span<T, Extent>(span), cleanup_fn_(std::move(cleanup_fn))
  {}
  ~OwningSpan()
  {
    if (cleanup_fn_)
      cleanup_fn_();
  }

  // Enable move and disable copy.
  OwningSpan(OwningSpan &&s) noexcept
    : std::span<T, Extent>(s), cleanup_fn_(std::move(s.cleanup_fn_))
  {}
  OwningSpan &operator=(OwningSpan &&s) noexcept
  {
    std::span<T, Extent>::operator=(s);
    cleanup_fn_ = std::move(s.cleanup_fn_);
    return *this;
  }
  OwningSpan(const OwningSpan &s) = delete;
  OwningSpan &operator=(const OwningSpan &s) = delete;

  explicit operator bool() const noexcept
  {
    return static_cast<bool>(cleanup_fn_);
  }

  // reset runs the cleanup function and makes the span empty
  void reset()
  {
    if (cleanup_fn_) {
      cleanup_fn_();
      cleanup_fn_ = UniqueFunction<void()>();
      std::span<T, Extent>::operator=(std::span<T, Extent>());
    }
  }

private:
  UniqueFunction<void()> cleanup_fn_;
};

// string_as_bytes converts a string into a OwningSpan of read-only bytes
// and transfers ownership of the underlying string.
//
// The is a useful conversion for legacy C++ code that stores byte buffers as
// strings.
inline OwningSpan<const std::byte>
string_as_bytes(std::string str)
{
  std::span<const std::byte> span = {
      reinterpret_cast<const std::byte *>(str.data()),
      str.size() * sizeof(decltype(str)::value_type)};
  return OwningSpan(span, [ref = std::move(str)] mutable {});
}

} // namespace hogd
