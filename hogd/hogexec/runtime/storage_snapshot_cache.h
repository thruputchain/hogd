#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

#include "contract.hh"
#include "hogd/base/hashmap.h"
#include "hogd/base/types.h"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

namespace detail {

struct CachedContract {
  SHA256Hash contract_hash_;
  std::shared_ptr<const Contract> contract_;

  std::span<const std::byte> get_key() const { return get_key(contract_hash_); }
  static std::span<const std::byte> get_key(SHA256Hash const &hash)
  {
    return std::as_bytes(std::span(hash));
  }
};

struct CachedDeployment {
  ContractAddress address_;
  SHA256Hash deployed_hash_;

  std::span<const std::byte> get_key() const { return get_key(address_); }
  static std::span<const std::byte> get_key(ContractAddress const &address)
  {
    return std::as_bytes(std::span(address));
  }
};

struct CachedStorageItem {
  AddressAndKey addrkey_;
  std::optional<StorageItem> item_;

  std::span<const std::byte> get_key() const { return get_key(addrkey_); }

  static std::span<const std::byte> get_key(AddressAndKey addrkey)
  {
    // AddressAndKey is a struct with two bytespans, laid out consecutively.
    const auto *addrkey_ptr = reinterpret_cast<const uint8_t *>(&addrkey);
    return std::as_bytes(std::span(addrkey_ptr, sizeof(AddressAndKey)));
  }
};

} // namespace detail

// Root cache for objects read from the ledger.
// Can be shared between concurrent transactions,
// but must be reset after each block.
//
// TODO(geoff): Not everything needs to be reset after every block.
// The key example is the cache of contracts.
// We might, therefore, move the contracts into some kind of LRU cache.
// Or in general change the caching policy of any type of object.
//
// TODO(geoff): These could be combined into one object, but then we need
// some mechanism to ensure domain separation between key types.
// This is possible with some e.g. higher level mechanism (if we can, for
// example, ensure that all ContractAddresses are different from deployed
// contract hashes), but that seems bugprone and difficult to maintain.
// Alternatively, we could construct a separate key type,
// although doing so without introducing excessive copies
// would require making HashMap have some internal knowledge of a notion of type
// (and still probably involves extra copies).
class StorageSnapshotCache {
public:
  std::shared_ptr<const Contract>
  try_get_contract_by_hash(SHA256Hash const &hash);
  std::optional<SHA256Hash>
  try_get_deployed_contract_hash(ContractAddress const &addr);
  std::optional<StorageItem>
  try_get_snapshot_item(AddressAndKey const &addrkey);

private:
  HashMap<detail::CachedContract> contract_cache_;
  HashMap<detail::CachedDeployment> deployment_cache_;
  HashMap<detail::CachedStorageItem> item_cache_;
};

} // namespace hogd
