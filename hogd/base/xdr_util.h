// xdr_util.h - utilities for converting to and from xdr types

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

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
    auto addr_view =
        std::string_view(reinterpret_cast<const char *>(addrkey.address.data()),
                         addrkey.address.size());
    auto key_disc = addrkey.key.type();
    std::string_view key_view = [&] {
      switch (key_disc) {
      case hogd::StorageKeyType::kStorageKeyTypeContractData:
        return std::string_view(reinterpret_cast<const char *>(
                                    addrkey.key.contract_data_key().data()),
                                addrkey.key.contract_data_key().size());
      case hogd::StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance:
        return std::string_view();
      }
      assert(false);
      std::unreachable();
    }();

    // This is the hash combine used in boost
    // https://www.boost.org/doc/libs/1_55_0/doc/html/hash/reference.html#boost.hash_combine
    auto hash_combine = []<typename T>(std::size_t &seed, T view) {
      std::hash<T> hasher;
      seed ^= hasher(view) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    };

    std::size_t seed = 0;
    hash_combine(seed, addr_view);
    hash_combine(seed, key_disc);
    hash_combine(seed, key_view);

    return seed;
  }
};
