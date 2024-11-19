// types.h - basic types used throughout the project

#pragma once

#include <cstddef>
#include <cstdint>

#include "types.hh"

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
static_assert(__SIZEOF_INT128__ == 16); // check for support

inline int128_xdr
to_xdr(int128_t value)
{
  return int128_xdr{.highbits = static_cast<int64_t>(value >> 64),
                    .lowbits = static_cast<uint64_t>(value & UINT64_MAX)};
}

inline uint128_xdr
to_xdr(uint128_t value)
{
  return uint128_xdr{.highbits = static_cast<uint64_t>(value >> 64),
                     .lowbits = static_cast<uint64_t>(value & UINT64_MAX)};
}

inline int128_t
from_xdr(int128_xdr value)
{
  return (static_cast<int128_t>(value.highbits) << 64) + value.lowbits;
}

inline uint128_t
from_xdr(uint128_xdr value)
{
  return (static_cast<uint128_t>(value.highbits) << 64) + value.lowbits;
}

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

} // namespace hogd
