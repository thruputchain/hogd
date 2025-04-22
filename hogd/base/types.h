// types.h - basic types used throughout the project

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace hogd {

using std::int16_t;
using std::int32_t;
using std::int64_t;
using std::int8_t;
using std::intptr_t;
using std::ptrdiff_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using std::uintptr_t;

// 128-bit integers (this is a GNU C++ extension supported by Clang and GCC)
using int128_t = __int128;
using uint128_t = unsigned __int128;
// NOLINTNEXTLINE(misc-redundant-expression)
static_assert(__SIZEOF_INT128__ == 16); // check for support

// Kibibytes (power of 2)
constexpr size_t
operator""_KiB(unsigned long long int kib)
{
  return kib * (1UL << 10);
}

// Mebibytes (power of 2)
constexpr size_t
operator""_MiB(unsigned long long int mib)
{
  return mib * (1UL << 20);
}

// Gigibytes (power of 2)
constexpr size_t
operator""_GiB(unsigned long long int gib)
{
  return gib * (1UL << 30);
}

// Wrap locations that work with nondeterministic values in this type;
// This should make auditing for nondeterminism easier.
template<typename T> class Nondeterministic {
public:
  template<typename... Args>
  Nondeterministic(Args &&...args)
    : nondeterministic_value_(std::forward<Args>(args)...)
  {}

  T const &get() { return nondeterministic_value_; }

private:
  T nondeterministic_value_;
};

} // namespace hogd
