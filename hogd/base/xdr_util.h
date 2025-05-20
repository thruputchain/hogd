// xdr_util.h - utilities for converting to and from xdr types

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include "hogd/base/types.h"
#include "types.hh"
#include "x23/serialize.h"

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
  return (int128_t{value.highbits} << 64) + value.lowbits;
}

constexpr uint128_t
from_xdr(uint128_xdr value)
{
  return (uint128_t{value.highbits} << 64) + value.lowbits;
}

} // namespace hogd

// enable hashing of important XDR types
template<std::size_t N> struct std::hash<std::array<uint8_t, N>> {
  std::size_t operator()(const std::array<uint8_t, N> &array) const noexcept
  {
    return std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char *>(array.data()), N));
  }
};

template<> struct std::hash<hogd::AddressAndKey> {
  std::size_t operator()(const hogd::AddressAndKey &addrkey) const noexcept
  {
    auto buf = x23::serialize(addrkey);
    return std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char *>(buf.data()),
                         buf.size()));
  }
};
