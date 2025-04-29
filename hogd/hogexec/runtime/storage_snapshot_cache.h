#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/base/xdr_util.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

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

  std::shared_ptr<const Contract>
  try_get_contract_by_hash(SHA256Hash const &hash);
  std::optional<SHA256Hash>
  try_get_deployed_contract_hash(ContractAddress const &addr);
  const StorageItem *try_get_snapshot_item(AddressAndKey const &addrkey);

private:
  // Note: the contract_cache_ does not need to be reset (although it is not
  // incorrect to reset it) after every block.
  // Future PRs will have a storage object cache, which will need to be reset.
  SynchronizedMap<SHA256Hash, std::shared_ptr<const Contract>> contract_cache_;
  // Neither does the deployment cache
  SynchronizedMap<ContractAddress, std::optional<SHA256Hash>> deployment_cache_;

  SynchronizedMap<AddressAndKey, std::unique_ptr<const StorageItem>>
      snapshot_item_cache_;

  const BlockID block_id_;
  DatabaseProxy &db_proxy_;
};

} // namespace hogd
