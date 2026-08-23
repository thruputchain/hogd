#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <utility>

#include "contract.hh"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/synchronized_map.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/item/snapshot_storage_item.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

// A cached contract: the bytecode plus, for translatable engines (rv32), its
// eagerly-built ready-to-instantiate image. `translated` is nullopt for
// non-rv32 contracts (and if translation failed -- treated as Unrecoverable by
// the engine, matching prior behavior).
struct ContractCacheEntry {
  Contract contract;
  std::optional<Rv32Contract> translated;
};

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

  // Each lookup answers from cache without allocating a coroutine frame --
  // MaybeAsync's value arm -- and falls back to an Async that suspends on the
  // database read only on a miss.  The lock guarding each cache is always
  // released before that read, so no cache lock is ever held across a
  // suspension.
  x23::MaybeAsync<SystemStatus<const Contract *>>
  try_get_contract_by_hash(SHA256Hash const &hash);
  x23::MaybeAsync<SystemStatus<const DeployedContract *>>
  try_get_deployed_contract(ContractAddress const &addr);
  x23::MaybeAsync<SystemStatus<const SnapshotStorageItem *>>
  try_get_snapshot_item(AddressAndKey const &addrkey);

  x23::MaybeAsync<SystemStatus<const DbTip *>> try_get_db_tip();

  ~StorageSnapshotCache();

  void reset()
  {
    contract_cache_.unsafe_clear();
    deployment_cache_.unsafe_clear();
    snapshot_item_cache_.unsafe_clear();
    block_id_ = UINT64_MAX;
    release_db_tip();
  }

  void set_block_id(BlockID block_id)
  {
    block_id_ = block_id;
    release_db_tip();
  }

private:
  void release_db_tip();
  // Lookup/insert/eager-translate the contract entry for `hash`. Returns
  // nullptr (wrapped) for a negative lookup.
  x23::MaybeAsync<SystemStatus<const ContractCacheEntry *>>
  get_contract_entry(SHA256Hash const &hash);

  // Miss paths for the lookups above.  Each takes its key BY VALUE so the key
  // is copied into the coroutine frame and stays valid across the database
  // read.
  x23::Async<SystemStatus<const ContractCacheEntry *>>
  load_contract_entry(SHA256Hash hash);
  x23::Async<SystemStatus<const Contract *>>
  load_contract_by_hash(SHA256Hash hash);
  x23::Async<SystemStatus<const DeployedContract *>>
  load_deployed_contract(ContractAddress addr);
  x23::Async<SystemStatus<const SnapshotStorageItem *>>
  load_snapshot_item(AddressAndKey addrkey);
  x23::Async<SystemStatus<const DbTip *>> load_db_tip();

  // Note: the contract_cache_ does not need to be reset (although it is not
  // incorrect to reset it) after every block.
  SynchronizedMap<SHA256Hash, std::optional<ContractCacheEntry>,
                  SipHash24Hasher, std::equal_to<>,
                  std::allocator<std::pair<const SHA256Hash,
                                           std::optional<ContractCacheEntry>>>,
                  std::shared_mutex>
      contract_cache_;
  // Neither does the deployment cache
  SynchronizedMap<ContractAddress, std::optional<DeployedContract>,
                  SipHash24Hasher, std::equal_to<>,
                  std::allocator<std::pair<const ContractAddress,
                                           std::optional<DeployedContract>>>,
                  std::shared_mutex>
      deployment_cache_;

  SynchronizedMap<
      AddressAndKey, SnapshotStorageItem, std::hash<AddressAndKey>,
      std::equal_to<>,
      std::allocator<std::pair<const AddressAndKey, SnapshotStorageItem>>,
      std::shared_mutex>
      snapshot_item_cache_;

  BlockID block_id_;
  DatabaseProxy &db_proxy_;

  std::atomic<DbTip *> db_tip_ = nullptr;
};

} // namespace hogd
