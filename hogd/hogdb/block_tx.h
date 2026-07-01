// block_tx.h - transaction handling logic at the granularity of a block.

#pragma once

#include <atomic>
#include <optional>
#include <utility>

#include "db_conflict.hh"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/concurrent_map.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/background_commit_worker.h"
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "types.hh"

namespace hogd {
// BlockTX manages a set of pending changes made in a block relative to the
// current snapshot state. They can be applied to the ramdb via commit().
// Multiple BlockTXes can be instantiated, but only one should be committed.
class BlockTX {
public:
  BlockTX(HogDBDAO &db, const StorageCostModel &cost_model,
          BackgroundCommitWorker &background_commit_worker,
          BlockHeight block_height, BlockExecutionMode mode)
    : db_(db), cost_model_(cost_model),
      background_commit_worker_(background_commit_worker),
      block_height_(block_height), mode_(mode)
  {}
  ~BlockTX() = default;

  // getters
  HogDBDAO &get_db() const { return db_; }
  BlockHeight get_height() const { return block_height_; }

  void invalidate() { invalidated_.store(true, std::memory_order_release); }
  bool is_invalidated() const
  {
    return invalidated_.load(std::memory_order_acquire);
  }

  void poison() { poisoned_.store(true, std::memory_order_release); }
  bool is_poisoned() const { return poisoned_.load(std::memory_order_acquire); }

  // commit applies the staged ChangeSet to the database.
  // Requires close() to have been called first.
  HogDBStatus<void> commit(WorkerPool &pool = WorkerPool::get_default_pool());

  // lock reserves the right to apply change to an item.
  // Lock returns the amount of storage tokens at least as large as what will
  // be needed to support the change, if committed.
  HogDBStatus<int128_t>
  lock(const ItemKey &key, ItemChange &change,
       std::optional<DbConflictCode> *conflict_reason = nullptr);
  // modify_lock modifies an existing change to an item that was reserved with
  // lock(). It could still fail if new_change has a conflict.
  // It returns an amount of storage tokens at least as large as what will
  // be needed to support the change, if committed.
  HogDBStatus<int128_t>
  modify_lock(const ItemKey &key, const ItemChange &old_change,
              const ItemChange &new_change,
              std::optional<DbConflictCode> *conflict_reason = nullptr);
  // unlock unreserves the right to apply change to an item and reverts it.
  void unlock(const ItemKey &key, ItemChange &change);
  // commit_change marks a change as committed.
  // returns the increase in committed storage cost.
  // Returns an error if the exclusive commit path reads a live DAO that was
  // mutated by a sibling block commit (stale block).
  HogDBStatus<int128_t> commit_change(const ItemKey &key, ItemChange change);

  void release_balance(const Address &addr, int128_t delta);

  HogDBStatus<int128_t> read_nnint128_base(const ItemKey &key);
  HogDBStatus<int128_t> read_balance_base(const Address &addr);

  // return entry from an RCQ that was created by a transaction in the current
  // block if it exists.
  HogDBStatus<std::optional<QueueEntry>>
  get_concurrent_rcq_entry(const ItemKey &key, uint32_t index);

  // Stage all finalized lock changes into a ChangeSet and compute the hash
  // delta. Returns the final db hash. Must be called before commit().
  HogDBStatus<DbHash> close(const DbHash &base_hash);

  // Test/benchmark only: stage changes without computing the hash.
  // Do not use in production code — the db hash is required for consensus.
  HogDBStatus<void> close_skip_hash_for_testing();

private:
  HogDBStatus<std::pair<int128_t, ItemLockCacheData>>
  get_snapshot_cost_and_cache_data(const ItemKey &key);

  HogDBStatus<void> check_stale() const
  {
    if (is_invalidated()) [[unlikely]] {
      return HogDBError(HogDBCode::kStaleBlock);
    }
    return {};
  }

  struct SipHash24Hasher {
    size_t operator()(const Address &key) const
    {
      static SipHash24 hasher;
      return hasher(key.as_bytes());
    }
    size_t operator()(const ItemKey &key) const
    {
      return std::hash<ItemKey>{}(key);
    }
  };

  HogDBDAO &db_;
  StorageCostModel cost_model_;
  ConcurrentMap<Address, ItemLock, SipHash24Hasher> addr_balance_locks_;
  ConcurrentMap<ItemKey, ItemLock, SipHash24Hasher> item_locks_;
  BackgroundCommitWorker &background_commit_worker_;
  BlockHeight block_height_;
  std::optional<ChangeSet> changeset_;
  std::atomic<bool> invalidated_{false};
  std::atomic<bool> poisoned_{false};
  const BlockExecutionMode mode_;

  HogDBStatus<void> stage_locks(ChangeSet &changes);

  static inline HogDBStatus<int128_t>
  do_lock(ItemLock &lock, const ItemChange &change, const LockContext &ctx);
  static inline HogDBStatus<int128_t>
  do_modify_lock(ItemLock &lock, const ItemChange &old_change,
                 const ItemChange &new_change, const LockContext &ctx);
  static inline void do_unlock_item(ItemLock &lock, const ItemChange &change,
                                    const LockContext &ctx);
};

// 192 on x86 (kCacheLineSize=64), 256 on aarch64 (kCacheLineSize=256).
static_assert(sizeof(ConcurrentMap<Address, ItemLock>::value_bucket_t) <= 256);
static_assert(sizeof(ConcurrentMap<ItemKey, ItemLock>::value_bucket_t) <= 256);

} // namespace hogd
