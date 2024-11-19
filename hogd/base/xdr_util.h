#pragma once

#include <cstdint>

#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

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

} // namespace hogd
