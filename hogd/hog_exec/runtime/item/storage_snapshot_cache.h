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

  SystemStatus<const Contract *>
  try_get_contract_by_hash(SHA256Hash const &hash);
  SystemStatus<const DeployedContract *>
  try_get_deployed_contract(ContractAddress const &addr);
  SystemStatus<const SnapshotStorageItem *>
  try_get_snapshot_item(AddressAndKey const &addrkey);

  SystemStatus<const DbTip *> try_get_db_tip();

  // The executing block's own facts, supplied by the scheduler when the block
  // was opened. Distinct from everything above, which is read from the db tip
  // -- and the tip, for an open block, is the *parent*. The height is
  // recoverable from the parent (it is the tip's height plus one) but the
  // timepoint is not: the parent's timepoint is the previous block's.
  //
  // Empty when no executor opened this block: the view path and the test and
  // bench harnesses build a snapshot cache directly. Callers must handle that
  // rather than assume a context is present.
  std::optional<BlockExecutionContext> const &block_execution_context() const
  {
    return block_execution_context_;
  }

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
    // Caches are pooled and reused across blocks, so this must be cleared
    // with the rest: a context surviving into the next block would serve it
    // the previous block's timepoint, silently and only under contracts that
    // read it.
    block_execution_context_.reset();
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
  SystemStatus<const ContractCacheEntry *>
  get_contract_entry(SHA256Hash const &hash);

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

  // Set once when the block is opened, before any transaction runs, and
  // cleared by reset() when the cache is recycled. Read-only for the life of
  // the block, so unlike the tip it needs no synchronization.
  std::optional<BlockExecutionContext> block_execution_context_;
  DatabaseProxy &db_proxy_;

  std::atomic<DbTip *> db_tip_ = nullptr;
};

} // namespace hogd
