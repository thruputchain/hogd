// types.h - basic types used throughout the project

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
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
using ssize_t = std::make_signed_t<size_t>;

// 128-bit integers (this is a GNU C++ extension supported by Clang and GCC)
using int128_t = __int128;
using uint128_t = unsigned __int128;
// NOLINTNEXTLINE(misc-redundant-expression)
static_assert(__SIZEOF_INT128__ == 16); // check for support

// #1846, #1874: lock down the bit layout that syscall memcpy and XDR
// conversion rely on. Compilation fails if any assumption breaks.
static_assert(
    [] {
      struct alignas(int128_t) B16 {
        unsigned char b[16];
      };
      // LE: least-significant byte first
      auto one = std::bit_cast<B16>(int128_t{1});
      if (one.b[0] != 1)
        return false;
      for (int i = 1; i < 16; ++i)
        if (one.b[i] != 0)
          return false;
      // two's complement: -1 is all-bits-set
      auto neg1 = std::bit_cast<B16>(int128_t{-1});
      // NOLINTNEXTLINE(modernize-loop-convert)
      for (int i = 0; i < 16; ++i)
        if (neg1.b[i] != 0xFF)
          return false;
      // high limb occupies bytes [8,15]
      auto high1 = std::bit_cast<B16>(int128_t{1} << 64);
      if (high1.b[8] != 1)
        return false;
      for (int i = 0; i < 8; ++i)
        if (high1.b[i] != 0)
          return false;
      for (int i = 9; i < 16; ++i)
        if (high1.b[i] != 0)
          return false;
      return true;
    }(),
    "int128_t must be little-endian two's complement");

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
  template<typename... Args> requires std::is_constructible_v<T, Args...>
  Nondeterministic(Args &&...args)
    : nondeterministic_value_(std::forward<Args>(args)...)
  {}

  auto &get(this auto &self) { return self.nondeterministic_value_; }

private:
  T nondeterministic_value_;
};

} // namespace hogd
