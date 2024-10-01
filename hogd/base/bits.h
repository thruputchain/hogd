// bits.h - bit hacks

#pragma once

#include <bit>
#include <cassert>
#include <compare>
#include <concepts>
#include <type_traits>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"

namespace hogd {

inline constexpr size_t kBitsPerByte = 8;

// align_up returns a value that is aligned upward to the nearest power of two.
template<std::unsigned_integral T>
constexpr T
align_up(T val, size_t align) noexcept
{
  ASSERT_CONSTEXPR(std::has_single_bit(align));
  return ((val + static_cast<T>(align) - 1) & ~static_cast<T>(align - 1));
}

// align_down returns a value that is aligned downward to the nearest power of
// two.
template<std::unsigned_integral T>
constexpr T
align_down(T val, size_t align) noexcept
{
  ASSERT_CONSTEXPR(std::has_single_bit(align));
  return (val & ~static_cast<T>(align - 1));
}

// divide_up performs division but rounds up to the next highest integer when
// there is a remainder.
template<std::integral T>
constexpr T
divide_up(T dividend, T divisor) noexcept
{
  return (dividend + divisor - 1) / divisor;
}

// WrappingSequence stores a monotonically increasing unsigned integer that is
// still valid for comparisons after overflow and wrap around.
//
// WARNING: The modular delta between any two WrappingSequence objects being
// compared must be smaller than the maximum signed value of T.
//
// This is an especially useful abstraction for sequence numbers like in TCP.
//
// Example:
//  WrappingSequence<uint8_t> s(0xff);
//  assert(s + 1 > s);
template<std::unsigned_integral T> struct WrappingSequence {
  T val;

  WrappingSequence() noexcept : val(0) {}
  explicit WrappingSequence(T val) noexcept : val(val) {}

  constexpr std::make_signed_t<T> operator-(const WrappingSequence &rhs) const
  {
    return static_cast<std::make_signed_t<T>>(val - rhs.val);
  }
  constexpr WrappingSequence operator+(const T &rhs) const
  {
    return WrappingSequence(val + rhs);
  }
  constexpr WrappingSequence &operator+=(const T &rhs)
  {
    val += rhs;
    return *this;
  }

  constexpr std::strong_ordering operator<=>(const WrappingSequence &rhs) const
  {
    auto cmp = static_cast<std::make_signed_t<T>>(val - rhs.val);
    if (cmp < 0)
      return std::strong_ordering::less;
    if (cmp == 0)
      return std::strong_ordering::equivalent;
    return std::strong_ordering::greater;
  }
  constexpr bool operator==(const WrappingSequence &rhs) const
  {
    return val == rhs.val;
  }
};

// TaggedPointer provides a pointer-like interface with additional tag bits
// that are ignored during dereferencing. It can be used to store 6 bits per
// pointer without additional storage overhead.
//
// TODO(amb): This class is compliant with Intel's LAM_U57 (6 bits), AMD's UAI
// (6 bits), and ARM's top-byte ignore (8 bits). On these CPUs, it could be
// made more efficient if we are willing to target them at compile time.
template<typename T> requires std::is_same_v<T, std::remove_reference_t<T>>
class TaggedPointer {
public:
  TaggedPointer() noexcept = default;
  explicit TaggedPointer(T *ptr) noexcept
    : ptr_and_tag_(reinterpret_cast<uintptr_t>(ptr))
  {}
  TaggedPointer(T *ptr, uint8_t tag) noexcept
    : ptr_and_tag_(reinterpret_cast<uintptr_t>(ptr) |
                   (static_cast<uintptr_t>(tag) << kAddressTagShift))
  {
    assert(tag < (1UL << kTagBits));
  }

  // operator overloads
  std::add_lvalue_reference_t<T> operator*() const noexcept
  {
    return *get_addr();
  }
  T *operator->() const noexcept { return get_addr(); }
  explicit operator bool() const noexcept { return get_addr() != nullptr; }
  TaggedPointer &operator=(T *ptr) noexcept
  {
    set_addr(ptr);
    return *this;
  }

  // setters and getters
  T *get() const { return get_addr(); }
  uint8_t get_tag() const
  {
    return static_cast<uint8_t>(ptr_and_tag_ >> kAddressTagShift);
  }
  void set_tag(uint8_t tag)
  {
    assert(tag < (1UL << kTagBits));
    ptr_and_tag_ &= kAddressMask;
    ptr_and_tag_ |= static_cast<uintptr_t>(tag) << kAddressTagShift;
  }

private:
  static constexpr int kAddressTagShift = 58;
  static constexpr int kTagBits = 6;
  static_assert(kAddressTagShift + kTagBits ==
                sizeof(uintptr_t) * kBitsPerByte);
  static constexpr uintptr_t kAddressMask = ((1UL << kAddressTagShift) - 1);
  static constexpr uintptr_t kTagMask = ~kAddressMask;

  uintptr_t ptr_and_tag_{0};

  T *get_addr() const
  {
    return reinterpret_cast<T *>(ptr_and_tag_ & kAddressMask); // NOLINT
  }
  void set_addr(T *ptr)
  {
    ptr_and_tag_ &= kTagMask;
    ptr_and_tag_ |= reinterpret_cast<uintptr_t>(ptr);
  }
};

template<typename T>
bool
operator==(TaggedPointer<T> tptr, T *ptr)
{
  return tptr.get() == ptr;
}

template<typename T>
bool
operator==(TaggedPointer<T> lhs, TaggedPointer<T> rhs)
{
  return lhs.get() == rhs.get();
}

} // namespace hogd
