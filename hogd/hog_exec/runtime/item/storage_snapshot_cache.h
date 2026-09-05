#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <limits>
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
#include "hogd/hog_exec/runtime/item/frame_item_key.h"
#include "hogd/hog_exec/runtime/item/snapshot_storage_item.h"
#include "hogd/hog_exec/runtime/prepared_contract.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

// A cached contract: the bytecode plus, for engines that prepare one, its
// eagerly-built ready-to-instantiate image. `prepared` is empty for engines
// with nothing to prepare (and if preparation failed -- treated as
// Unrecoverable by the engine, matching prior behavior).
struct ContractCacheEntry {
  Contract contract;
  PreparedContract prepared;
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
  StorageSnapshotCache(BlockID block_id,
                       BlockExecutionContext const &block_execution,
                       DatabaseProxy &db_proxy);
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
  //
  // The cached contract for `hash`: its code and the image this cache built
  // when it loaded it, so callers reuse that image rather than preparing the
  // same bytes again. Borrowed -- node-stable and valid for the block, so it
  // outlives any transaction's runtimes. Returns nullptr (wrapped) for a
  // negative lookup.
  x23::MaybeAsync<SystemStatus<const ContractCacheEntry *>>
  try_get_contract_entry(SHA256Hash const &hash);
  x23::MaybeAsync<SystemStatus<const DeployedContract *>>
  try_get_deployed_contract(ContractAddress const &addr);
  // The snapshot item under `key`, probed with the hash the key already
  // carries. The syscall path hands over the FrameItemKey it built for its
  // frame probe, so a first touch lays out and hashes the key once for the
  // frames and this cache together.
  x23::MaybeAsync<SystemStatus<const SnapshotStorageItem *>>
  try_get_snapshot_item(FrameItemKey const &key);
  // The XDR-keyed form, for a caller holding no frame key. Lays out and
  // hashes the key here; nothing on the syscall path should take this.
  x23::MaybeAsync<SystemStatus<const SnapshotStorageItem *>>
  try_get_snapshot_item(AddressAndKey const &addrkey)
  {
    return try_get_snapshot_item(FrameItemKey::from(addrkey));
  }

  x23::MaybeAsync<SystemStatus<const DbTip *>> try_get_db_tip();

  // The executing block's own facts. Unlike everything above these do not
  // come from the db tip, which for an open block is the *parent*: the height
  // is the tip's plus one, and the timepoint is not derivable from it at all.
  BlockExecutionContext const &block_execution_context() const
  {
    return block_execution_context_;
  }

  // Re-arms a recycled cache for its next block.
  void set_block_execution_context(BlockExecutionContext const &block_execution)
  {
    block_execution_context_ = block_execution;
  }

  ~StorageSnapshotCache();

  void reset()
  {
    contract_cache_.unsafe_clear();
    deployment_cache_.unsafe_clear();
    snapshot_item_cache_.unsafe_clear();
    block_id_ = UINT64_MAX;
    // Poison rather than leave the last block's context behind:
    // get_snapshot_cache() re-arms it, and anything that reads it before then
    // gets an obviously-wrong height rather than a plausible stale one.
    block_execution_context_ = BlockExecutionContext{
        .block_height = std::numeric_limits<BlockHeight>::min(),
        .block_timepoint = 0,
    };
    release_db_tip();
  }

  void set_block_id(BlockID block_id)
  {
    block_id_ = block_id;
    release_db_tip();
  }

private:
  void release_db_tip();

  // Miss paths for the lookups above.  Each takes its key BY VALUE so the key
  // is copied into the coroutine frame and stays valid across the database
  // read.
  x23::Async<SystemStatus<const ContractCacheEntry *>>
  load_contract_entry(SHA256Hash hash);
  x23::Async<SystemStatus<const DeployedContract *>>
  load_deployed_contract(ContractAddress addr);
  x23::Async<SystemStatus<const SnapshotStorageItem *>>
  load_snapshot_item(FrameItemKey key);
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

  // Keyed by the frame key rather than the XDR AddressAndKey: the hasher
  // returns the hash cached in the key, so a probe costs one shard index and
  // one bucket index off a value already in hand, and a rehash as the cache
  // grows moves keys instead of re-serializing and re-hashing every one.
  // Same process-keyed SipHash24 as before; the key is contract-chosen and
  // the map has to stay flood-resistant. The XDR form is rebuilt once, on a
  // miss, for the database read and the item.
  SynchronizedMap<
      FrameItemKey, SnapshotStorageItem, FrameItemKeyHash, std::equal_to<>,
      std::allocator<std::pair<const FrameItemKey, SnapshotStorageItem>>,
      std::shared_mutex>
      snapshot_item_cache_;

  BlockID block_id_;

  // Read-only for the life of the block, so unlike the tip it needs no
  // synchronization.
  BlockExecutionContext block_execution_context_;
  DatabaseProxy &db_proxy_;

  std::atomic<DbTip *> db_tip_ = nullptr;
};

} // namespace hogd
