#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/concurrent_map.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/status.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

struct Contract;
class DatabaseProxy;

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
  StorageSnapshotCache(BlockID block_id, DatabaseProxy &db_proxy);
  // delete move and copy
  StorageSnapshotCache(StorageSnapshotCache const &) = delete;
  StorageSnapshotCache(StorageSnapshotCache &&) = delete;
  StorageSnapshotCache &operator=(StorageSnapshotCache const &) = delete;
  StorageSnapshotCache &operator=(StorageSnapshotCache &&) = delete;

  SystemStatus<std::shared_ptr<const Contract>>
  try_get_contract_by_hash(SHA256Hash const &hash);
  SystemStatus<std::optional<SHA256Hash>>
  try_get_deployed_contract_hash(ContractAddress const &addr);
  SystemStatus<SnapshotStorageItem *>
  try_get_snapshot_item(AddressAndKey const &addrkey);

  ~StorageSnapshotCache();

private:
  struct DatabaseCacheEntry {
    DatabaseCacheEntry(std::shared_ptr<const Contract> contract) : data_(contract) {}
    DatabaseCacheEntry(std::optional<SHA256Hash> hash) : data_(hash) {}
    DatabaseCacheEntry(StorageItem item, AddressAndKey const& addrkey, BlockID block_id, DatabaseProxy &db_proxy)
     : data_(std::in_place_type<SnapshotStorageItem>, std::move(item), addrkey, block_id, db_proxy) {}
    
    std::variant<std::shared_ptr<const Contract>, std::optional<SHA256Hash>, SnapshotStorageItem> data_;
  };

  // ContractAddress and SHA256Hash are both std::array<uint8_t, 32>,
  // so we declare separate types for each different key.
  struct DeploymentCacheKey {
    ContractAddress addr_;
    bool operator==(DeploymentCacheKey const &other) const = default;
  };

  struct ContractCacheKey {
    SHA256Hash hash_;
    bool operator==(ContractCacheKey const &other) const = default;
  };

  using CacheKey = std::variant<ContractCacheKey, DeploymentCacheKey, AddressAndKey>;

  struct CacheKeyHash {
    size_t operator()(CacheKey const &key) const {
      static SipHash24 hasher;
      return std::visit([&](auto &&arg) {
        if constexpr (std::is_same_v<std::decay_t<decltype(arg)>, ContractCacheKey>) {
          return hasher(arg.hash_);
        } else if constexpr (std::is_same_v<std::decay_t<decltype(arg)>, DeploymentCacheKey>) {
          return hasher(arg.addr_);
        } else {
          static_assert(std::is_same_v<std::decay_t<decltype(arg)>, AddressAndKey>);
          auto buf = x23::serialize(arg);
          return hasher(buf);
        }
      }, key);
    }
  };

  ConcurrentMap<CacheKey, DatabaseCacheEntry, CacheKeyHash> cache_;

  const BlockID block_id_;
  DatabaseProxy &db_proxy_;
};

} // namespace hogd
