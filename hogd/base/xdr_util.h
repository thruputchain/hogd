// xdr_util.h - utilities for converting to and from xdr types

#pragma once

#include <array>
#include <cstdint>

#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

constexpr int128_xdr
to_xdr(int128_t value)
{
  return int128_xdr{.highbits = static_cast<int64_t>(value >> 64),
                    .lowbits = static_cast<uint64_t>(value & UINT64_MAX)};
}

constexpr uint128_xdr
to_xdr(uint128_t value)
{
  return uint128_xdr{.highbits = static_cast<uint64_t>(value >> 64),
                     .lowbits = static_cast<uint64_t>(value & UINT64_MAX)};
}

constexpr int128_t
from_xdr(int128_xdr value)
{
  return (static_cast<int128_t>(value.highbits) << 64) + value.lowbits;
}

constexpr uint128_t
from_xdr(uint128_xdr value)
{
  return (static_cast<uint128_t>(value.highbits) << 64) + value.lowbits;
}

} // namespace hogd

template<std::size_t N> struct std::hash<std::array<uint8_t, N>> {
  std::size_t operator()(const std::array<uint8_t, N> &array) const noexcept
  {
    return std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char *>(array.data()), N));
  }
};
