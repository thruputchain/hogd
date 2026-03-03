#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

#include "hogd/crypto/lthash16.h"
#include "hogd/hogdb/hogdb_dao.h"

namespace hogd {

inline bool
is_state_prefix(std::byte prefix)
{
  switch (static_cast<Prefix>(prefix)) {
  case Prefix::kBalance:
  case Prefix::kAddressAuthData:
  case Prefix::kAddressMetadata:
  case Prefix::kContractDataKey:
  case Prefix::kWasmCodeRefcount:
  case Prefix::kWasmCode:
  case Prefix::kReplayCache:
  case Prefix::kRetrocausalQueueBlock:
    return true;
  case Prefix::kHogAge:
  case Prefix::kGlobalMetadata:
    return false;
  default:
    return false;
  }
}

inline void
lthash_add_item(LtHash16 &hash, std::span<const std::byte> key, uint8_t type,
                std::span<const std::byte> value)
{
  if (key.empty() || !is_state_prefix(key[0])) {
    return;
  }
  std::vector<std::byte> element;
  element.reserve(key.size() + 1 + value.size());
  element.insert(element.end(), key.begin(), key.end());
  element.push_back(static_cast<std::byte>(type));
  element.insert(element.end(), value.begin(), value.end());
  hash.add(element);
}

inline void
lthash_remove_item(LtHash16 &hash, std::span<const std::byte> key, uint8_t type,
                   std::span<const std::byte> value)
{
  if (key.empty() || !is_state_prefix(key[0])) {
    return;
  }
  std::vector<std::byte> element;
  element.reserve(key.size() + 1 + value.size());
  element.insert(element.end(), key.begin(), key.end());
  element.push_back(static_cast<std::byte>(type));
  element.insert(element.end(), value.begin(), value.end());
  hash.remove(element);
}

inline void
replace_known_item(LtHash16 &hash, HogDBDAO &db, std::span<const std::byte> key,
                   uint8_t type, std::span<const std::byte> new_value)
{
  auto before = db.get_ramdb().get(key);
  if (before.has_value()) {
    lthash_remove_item(hash, key, before->type, before->value);
  }
  lthash_add_item(hash, key, type, new_value);
}

inline auto
make_address_key_with_prefix(const Address &addr, Prefix prefix)
{
  std::array<std::byte, kAddressSize + 1> ret{};
  ret[0] = static_cast<std::byte>(prefix);
  std::ranges::copy(addr.as_bytes(),
                    std::begin(std::as_writable_bytes(std::span(ret))) + 1);
  return ret;
}

inline auto
make_contract_data_db_key(std::span<const std::byte> key)
{
  std::vector<std::byte> out;
  out.reserve(key.size() + 1);
  out.push_back(static_cast<std::byte>(Prefix::kContractDataKey));
  out.insert(out.end(), key.begin(), key.end());
  return out;
}

inline auto
make_wasm_code_db_key(const SHA256Hash &hash)
{
  std::array<std::byte, std::tuple_size_v<SHA256Hash> + 1> wasm_key{};
  wasm_key[0] = static_cast<std::byte>(Prefix::kWasmCode);
  std::ranges::copy(std::as_bytes(std::span(hash)),
                    std::begin(std::span(wasm_key)) + 1);
  return wasm_key;
}

} // namespace hogd
