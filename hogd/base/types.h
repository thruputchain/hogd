// types.h - basic types used throughout the project

#pragma once

#include <cstddef>
#include <cstdint>

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
