// hogd_types.h - defines the basic data types used in the HOG blockchain.

#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <format>
#include <span>

#include "hogd/base/bits.h"
#include "hogd/base/span.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"

namespace hogd {

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

private:
  friend struct std::formatter<Address>;

  const RawAddress addr_;
};

[[nodiscard]] inline Address
make_address(std::string_view str)
{
  return Address(sha256(byte_view(str.data(), str.size())));
}

[[nodiscard]] constexpr Address
make_address(std::integral auto val)
{
  RawAddress buf{};
  std::ranges::copy(byte_view(cpu_to_le(val)),
                    std::begin(std::as_writable_bytes(std::span(buf))));
  return Address(buf);
}

// ItemKey identifies an on-chain storage item for an Address.
class ItemKey {
public:
  constexpr ItemKey(Address addr, RawKey key)
    : item_key_{.addr = addr, .key = key},
      memoized_hash_(hogd::SipHash24::get_default()(as_bytes()))
  {}
  [[nodiscard]] constexpr std::span<const std::byte> as_bytes() const
  {
    return byte_view(item_key_);
  }
  constexpr bool operator==(const ItemKey &key) const
  {
    return item_key_.addr == key.item_key_.addr &&
           item_key_.key == key.item_key_.key;
  }

private:
  friend struct std::formatter<ItemKey>;
  friend struct std::hash<ItemKey>;

  const struct {
    Address addr;
    RawKey key;
  } item_key_;
  const size_t memoized_hash_;
};

[[nodiscard]] inline ItemKey
make_item_key(Address addr, std::string_view str)
{
  return ItemKey(addr, sha256(byte_view(str.data(), str.size())));
}

[[nodiscard]] constexpr ItemKey
make_item_key(Address addr, std::integral auto val)
{
  RawKey buf{};
  std::ranges::copy(byte_view(cpu_to_le(val)),
                    std::begin(std::as_writable_bytes(std::span(buf))));
  return ItemKey(addr, buf);
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
