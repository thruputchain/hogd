// hogd_types.h - defines the basic data types used in the HOG blockchain.

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <format>
#include <functional>
#include <span>
#include <string_view>
#include <utility>

#include "hogd/base/bits.h"
#include "hogd/base/span.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

// a handle for a block transaction.
using BlockTXID = uint64_t;
// a handle for an invocation's transaction.
using InvokeTXID = uint64_t;

} // namespace hogd

template<> struct std::hash<std::pair<hogd::BlockTXID, hogd::InvokeTXID>> {
  [[nodiscard, gnu::pure]] static std::size_t
  operator()(const std::pair<hogd::BlockTXID, hogd::InvokeTXID> &p) noexcept
  {
    return std::hash<hogd::BlockTXID>()(p.first) ^
           std::hash<hogd::InvokeTXID>()(p.second);
  }
};

namespace hogd {

// storage_cost returns the cost in hogs for storage.
//
// WARNING: This does not necessarily reflect the real storage cost. It can't
// easily be changed because it is part of the blockchain model.
constexpr size_t
storage_cost(size_t bytes)
{
  constexpr size_t kFixedCost = 128; // for the key and metadata
  return kFixedCost + bytes;
}

inline constexpr size_t kAddressSize = 32;
inline constexpr size_t kItemKeySize = 32;

// TODO(abelay): use XDR definitions for these.
using RawAddress = std::array<uint8_t, kAddressSize>;
using RawKey = std::array<uint8_t, kItemKeySize>;

// Address identifies an on-chain address that contains at least a balance and
// other metadata. It may also contain storage items and smart contract code.
class Address {
public:
  constexpr Address(RawAddress addr) : addr_(addr) {}
  [[nodiscard]] constexpr std::span<const std::byte> as_bytes() const
  {
    return std::as_bytes(std::span(addr_));
  }
  constexpr bool operator==(const Address &addr) const = default;

  [[nodiscard]] constexpr auto get_raw() const { return addr_; }

private:
  friend struct std::formatter<Address>;

  const RawAddress addr_;
};

[[nodiscard]] inline Address
make_address(std::string_view str)
{
  return Address(sha256(std::as_bytes(std::span(str))));
}

[[nodiscard]] constexpr Address
make_address(std::integral auto val)
{
  RawAddress buf{};
  std::ranges::copy(byte_view(cpu_to_le(val)),
                    std::begin(std::as_writable_bytes(std::span(buf))));
  return Address(buf);
}

// This is a superset of the StorageKeyTypes.
// Every StorageKeyType belongs to an Address,
// but some ItemTypes, like wasm code, do not.
enum class ItemKeyType {
  kItemKeyTypeContractData = 0,
  kItemKeyTypeFreeNativeTokenBalance = 1,
  kItemKeyTypeAccountAuthData = 2,
  kItemKeyTypeAddressMetadata = 3,
  kItemKeyTypeContractCode = 4,
  kItemKeyTypeContractRefcount = 5,
};

} // namespace hogd

template<> struct std::formatter<hogd::ItemKeyType> {
  constexpr static auto parse(format_parse_context &ctx) { return ctx.begin(); }

  static auto format(const hogd::ItemKeyType &type, format_context &ctx)
  {
    switch (type) {
    case hogd::ItemKeyType::kItemKeyTypeContractData:
      return format_to(ctx.out(), "kItemKeyTypeContractData");
    case hogd::ItemKeyType::kItemKeyTypeFreeNativeTokenBalance:
      return format_to(ctx.out(), "kItemKeyTypeFreeNativeTokenBalance");
    case hogd::ItemKeyType::kItemKeyTypeAccountAuthData:
      return format_to(ctx.out(), "kItemKeyTypeAccountAuthData");
    case hogd::ItemKeyType::kItemKeyTypeAddressMetadata:
      return format_to(ctx.out(), "kItemKeyTypeAddressMetadata");
    case hogd::ItemKeyType::kItemKeyTypeContractCode:
      return format_to(ctx.out(), "kItemKeyTypeContractCode");
    case hogd::ItemKeyType::kItemKeyTypeContractRefcount:
      return format_to(ctx.out(), "kItemKeyTypeContractRefcount");
    }
    return format_to(ctx.out(), "Unknown");
  }
};

namespace hogd {

// ItemKey identifies an on-chain storage item.
class ItemKey {
public:
  constexpr ItemKey(
      Address addr, RawKey key,
      ItemKeyType key_type = ItemKeyType::kItemKeyTypeContractData)
    : item_key_{.addr = addr, .key = key}, key_type_(key_type),
      memoized_hash_(hogd::SipHash24::get_default()(as_bytes()))
  {}
  [[nodiscard]] constexpr std::span<const std::byte> as_bytes() const
  {
    switch (key_type_) {
    // ItemKeyTypes that do not use the .key field
    case ItemKeyType::kItemKeyTypeFreeNativeTokenBalance:
    case ItemKeyType::kItemKeyTypeAccountAuthData:
    case ItemKeyType::kItemKeyTypeAddressMetadata:
    // These ItemKeyTypes use the .addr field, but as a SHA256Hash,
    // not an Address.
    case ItemKeyType::kItemKeyTypeContractCode:
    case ItemKeyType::kItemKeyTypeContractRefcount:
      return item_key_.addr.as_bytes();
    case ItemKeyType::kItemKeyTypeContractData:
      return byte_view(item_key_);
    }
    std::unreachable();
  }
  constexpr bool operator==(const ItemKey &key) const
  {
    if (key_type_ != key.key_type_)
      return false;
    switch (key_type_) {
    // ItemKeyTypes that do not use the .key field
    case ItemKeyType::kItemKeyTypeFreeNativeTokenBalance:
    case ItemKeyType::kItemKeyTypeAccountAuthData:
    case ItemKeyType::kItemKeyTypeAddressMetadata:
    // These ItemKeyTypes use the .addr field, but as a SHA256Hash,
    // not an Address.
    case ItemKeyType::kItemKeyTypeContractCode:
    case ItemKeyType::kItemKeyTypeContractRefcount:
      return item_key_.addr == key.item_key_.addr;
    case ItemKeyType::kItemKeyTypeContractData:
      return item_key_.addr == key.item_key_.addr &&
             item_key_.key == key.item_key_.key;
    }
    std::unreachable();
  }

  constexpr bool is_address_balance() const
  {
    return key_type_ == ItemKeyType::kItemKeyTypeFreeNativeTokenBalance;
  }
  constexpr bool is_account_auth_data() const
  {
    return key_type_ == ItemKeyType::kItemKeyTypeAccountAuthData;
  }
  constexpr bool is_address_metadata() const
  {
    return key_type_ == ItemKeyType::kItemKeyTypeAddressMetadata;
  }
  constexpr bool is_contract_data() const
  {
    return key_type_ == ItemKeyType::kItemKeyTypeContractData;
  }
  constexpr bool is_contract_code() const
  {
    return key_type_ == ItemKeyType::kItemKeyTypeContractCode;
  }
  constexpr bool is_contract_refcount() const
  {
    return key_type_ == ItemKeyType::kItemKeyTypeContractRefcount;
  }
  [[nodiscard]] constexpr Address get_address() const { return item_key_.addr; }

  [[nodiscard]] constexpr SHA256Hash get_contract_code_hash() const
  {
    assert(is_contract_code() || is_contract_refcount());
    return item_key_.addr.get_raw();
  }
  [[nodiscard]] constexpr Address get_key() const { return item_key_.key; }

  constexpr auto key_type() const { return key_type_; }

private:
  friend struct std::formatter<ItemKey>;
  friend struct std::hash<ItemKey>;

  const struct {
    Address addr;
    RawKey key;
  } item_key_;
  ItemKeyType key_type_;
  const size_t memoized_hash_;
};

[[nodiscard]] inline ItemKey
make_item_key(const Address &addr, std::string_view str)
{
  return ItemKey(addr, sha256(byte_view(str.data(), str.size())));
}

[[nodiscard]] constexpr ItemKey
make_item_key(const Address &addr, std::integral auto val)
{
  RawKey buf{};
  std::ranges::copy(byte_view(cpu_to_le(val)),
                    std::begin(std::as_writable_bytes(std::span(buf))));
  return ItemKey(addr, buf);
}

[[nodiscard]] constexpr ItemKey
make_address_balance_item_key(const Address &addr)
{
  RawKey buf{};
  return ItemKey(addr, buf, ItemKeyType::kItemKeyTypeFreeNativeTokenBalance);
}

[[nodiscard]] constexpr ItemKey
make_account_auth_data_item_key(const Address &addr)
{
  RawKey buf{};
  return ItemKey(addr, buf, ItemKeyType::kItemKeyTypeAccountAuthData);
}

[[nodiscard]] constexpr ItemKey
make_address_metadata_item_key(const Address &addr)
{
  RawKey buf{};
  return ItemKey(addr, buf, ItemKeyType::kItemKeyTypeAddressMetadata);
}

[[nodiscard]] constexpr ItemKey
make_contract_code_item_key(SHA256Hash const &hash)
{
  RawKey buf{};
  return ItemKey(hash, buf, ItemKeyType::kItemKeyTypeContractCode);
}

[[nodiscard]] constexpr ItemKey
make_contract_refcount_item_key(SHA256Hash const &hash)
{
  RawKey buf{};
  return ItemKey(hash, buf, ItemKeyType::kItemKeyTypeContractRefcount);
}

[[nodiscard]] constexpr ItemKey
make_item_key(const AddressAndKey &addrkey)
{
  switch (addrkey.key.key_type()) {
  case StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance:
    return make_address_balance_item_key(addrkey.address);
  case StorageKeyType::kStorageKeyTypeAccountAuthData:
    return make_account_auth_data_item_key(addrkey.address);
  case StorageKeyType::kStorageKeyTypeAddressMetadata:
    return make_address_metadata_item_key(addrkey.address);
  case StorageKeyType::kStorageKeyTypeContractData:
    return ItemKey(addrkey.address, addrkey.key.contract_data_key());
  }
  std::unreachable();
}

} // namespace hogd

template<> struct std::formatter<hogd::Address> {
  static constexpr auto parse(std::format_parse_context &ctx)
  {
    return ctx.begin();
  }
  static auto format(const hogd::Address &addr, std::format_context &ctx)
  {
    return std::format_to(ctx.out(), "G{}",
                          hogd::encode_base64_url_safe(
                              std::as_bytes(std::span(addr.addr_))));
  }
};

template<> struct std::hash<hogd::Address> {
  [[nodiscard, gnu::pure]] std::size_t
  operator()(const hogd::Address &addr) const noexcept
  {
    return hogd::SipHash24::get_default()(addr.as_bytes());
  }
};

template<> struct std::formatter<hogd::ItemKey> {
  static constexpr auto parse(std::format_parse_context &ctx)
  {
    return ctx.begin();
  }
  static auto format(const hogd::ItemKey &key, std::format_context &ctx)
  {
    if (key.is_address_balance())
      return std::format_to(ctx.out(), "{}:balance", key.item_key_.addr);
    return std::format_to(ctx.out(), "{}:{}", key.item_key_.addr,
                          hogd::encode_base64_url_safe(
                              std::as_bytes(std::span(key.item_key_.key))));
  }
};

template<> struct std::hash<hogd::ItemKey> {
  [[nodiscard, gnu::pure]] std::size_t
  operator()(const hogd::ItemKey &key) const noexcept
  {
    return key.memoized_hash_;
  }
};
