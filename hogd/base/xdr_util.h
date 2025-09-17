// xdr_util.h - utilities for converting to and from xdr types

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>
#include <version>

#include "hogd/base/bits.h"
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
  return (int128_t{value.highbits} << 64) + value.lowbits;
}

constexpr uint128_t
from_xdr(uint128_xdr value)
{
  return (uint128_t{value.highbits} << 64) + value.lowbits;
}

template<typename T>
constexpr T
xdr_le_to_cpu(T value)
{
  static_assert(
      std::is_same_v<T, int128_xdr> || std::is_same_v<T, const int128_xdr> ||
          std::is_same_v<T, uint128_xdr> ||
          std::is_same_v<T, const uint128_xdr>,
      "[T] must be [int128_xdr] or [uint128_xdr] (const or non-const).");

  value.highbits = le_to_cpu(value.highbits);
  value.lowbits = le_to_cpu(value.lowbits);
  return value;
}

template<typename T>
constexpr T
xdr_cpu_to_le(T value)
{
  static_assert(
      std::is_same_v<T, int128_xdr> || std::is_same_v<T, const int128_xdr> ||
          std::is_same_v<T, uint128_xdr> ||
          std::is_same_v<T, const uint128_xdr>,
      "[T] must be [int128_xdr] or [uint128_xdr] (const or non-const).");

  value.highbits = cpu_to_le(value.highbits);
  value.lowbits = cpu_to_le(value.lowbits);
  return value;
}

template<typename T>
constexpr T
xdr_be_to_cpu(T value)
{
  static_assert(
      std::is_same_v<T, int128_xdr> || std::is_same_v<T, const int128_xdr> ||
          std::is_same_v<T, uint128_xdr> ||
          std::is_same_v<T, const uint128_xdr>,
      "[T] must be [int128_xdr] or [uint128_xdr] (const or non-const).");

  value.highbits = be_to_cpu(value.highbits);
  value.lowbits = be_to_cpu(value.lowbits);
  return value;
}

template<typename T>
constexpr T
xdr_cpu_to_be(T value)
{
  static_assert(
      std::is_same_v<T, int128_xdr> || std::is_same_v<T, const int128_xdr> ||
          std::is_same_v<T, uint128_xdr> ||
          std::is_same_v<T, const uint128_xdr>,
      "[T] must be [int128_xdr] or [uint128_xdr] (const or non-const).");

  value.highbits = cpu_to_be(value.highbits);
  value.lowbits = cpu_to_be(value.lowbits);
  return value;
}

} // namespace hogd

// Format ranges is not supported until gcc 15.2
#if (__cpp_lib_format_ranges < 202207L)

#include "hogd/base/string.h"
#include <format>

template<size_t N> struct std::formatter<std::array<uint8_t, N>> {
  static constexpr auto parse(std::format_parse_context &ctx)
  {
    return ctx.begin();
  }
  static auto format(const std::array<uint8_t, N> &array,
                     std::format_context &ctx)
  {
    return std::format_to(ctx.out(), "{}",
                          hogd::encode_hex(std::as_bytes(std::span(array))));
  }
};

#endif

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
    auto key_disc = addrkey.key.key_type();
    std::string_view key_view = [&] {
      switch (key_disc) {
      case hogd::StorageKeyType::kStorageKeyTypeContractData:
        return std::string_view(reinterpret_cast<const char *>(
                                    addrkey.key.contract_data_key().data()),
                                addrkey.key.contract_data_key().size());
      case hogd::StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance:
      case hogd::StorageKeyType::kStorageKeyTypeAccountAuthMetadata:
      case hogd::StorageKeyType::kStorageKeyTypeAddressMetadata:
        return std::string_view();
      case hogd::StorageKeyType::kStorageKeyTypeContractDeploymentMetadata:
        return std::string_view(reinterpret_cast<const char *>(
                                    addrkey.key.contract_hash().data()),
                                addrkey.key.contract_hash().size());
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
