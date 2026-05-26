#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/auth_lock.h"
#include "hogd/hogdb/item_lock/balance_lock.h"
#include "hogd/hogdb/item_lock/bytestring_lock.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/deletion_lock.h"
#include "hogd/hogdb/item_lock/exclusive_lock.h"
#include "hogd/hogdb/item_lock/new_contract_lock.h"
#include "hogd/hogdb/item_lock/new_deployment_lock.h"
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/item_lock/replay_cache_lock.h"
#include "hogd/hogdb/item_lock/retrocausal_queue_lock.h"
#include "hogd/hogdb/status.h"
#include "x23/testlock.h"

namespace hogd {

class ItemLock {
public:
  ItemLock(int128_t snapshot_cost) : snapshot_cost_(snapshot_cost) {}
  explicit ItemLock(std::pair<int128_t, ItemLockCacheData> const &snapshot)
  {
    set_snapshot_data(snapshot);
  }
  ItemLock(auto const &lambda)
      requires std::invocable<decltype(lambda)> &&
               std::same_as<std::invoke_result_t<decltype(lambda)>,
                            HogDBStatus<std::pair<int128_t, ItemLockCacheData>>>
  {
    auto status = lambda();
    if (!status) [[unlikely]] {
      // We need to actually panic here; we do not want to allow
      // a corrupted cache entry to get read as valid or kHogDBCode::kNotFound.
      // TODO(geoff): We could consider explicitly marking the cache entry
      // as corrupted, and handling errors more gracefully.
      panic("failed to get snapshot cost: {}", status.error());
      // any action on this item will fail
      snapshot_cost_ = std::numeric_limits<int128_t>::min();
      return;
    }
    set_snapshot_data(*status);
  }

  // If after!=nullptr, returns a deterministic upper bound on the storage cost
  // required to support the after change. Is a no-op on failure.
  HogDBStatus<int128_t> try_atomic_change_lock(const ItemChange *before,
                                               const ItemChange *after,
                                               const LockContext &ctx);

  // Commits the change to the item lock.
  // Returns the storage cost used to commit the change.
  // Must be less than or equal to the storage cost returned by
  // try_atomic_change_lock for that change.
  // change must be non-null.
  // Returns an error if the exclusive commit path re-validates against a
  // live DAO that was mutated by a sibling block commit.
  HogDBStatus<int128_t> commit_change(ItemChange change,
                                      const LockContext &ctx);

  void add_committed_balance_credit(int128_t delta);

  HogDBStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

  std::optional<QueueEntry> get_concurrent_entry(uint32_t index) const;

  bool is_finalized() const
  {
    return lock_state_.load(std::memory_order_acquire) != kUncommitted;
  }

  // not at all threadsafe.  Only use after all txs in a block are done.
  template<typename T> const T *get_inner() const
  {
    assert(lock_state_.load(std::memory_order_relaxed) == kFinalized ||
           lock_state_.load(std::memory_order_relaxed) ==
               kFinalizedPermamentExclusiveLock);
    return std::get_if<T>(&lock_contents_);
  }

  std::optional<int128_t> get_cached_nnint128_base() const
  {
    if (snapshot_type_ != kItemLockCacheDataNNInt128BaseValue) {
      return std::nullopt;
    }
    return nnint_base_;
  }

private:
  HogDBStatus<int128_t>
  handle_try_atomic_from_finalized(const ItemChange *before,
                                   const ItemChange *after,
                                   const LockContext &ctx);
  HogDBStatus<int128_t> handle_try_atomic_from_pending(
      const ItemChange *before, const ItemChange *after, const LockContext &ctx,
      std::unique_lock<x23::TestLock> const &ul);

  // No change has committed on this item, and we would like to try to acquire a
  // change in a transaction that does not currently hold any lock on this item.
  HogDBStatus<int128_t>
  handle_uncommitted_try_new_lock(const ItemChange *after,
                                  const LockContext &ctx,
                                  std::unique_lock<x23::TestLock> &ul);
  void
  handle_release_uncommitted_lock(const ItemChange *before,
                                  const LockContext &ctx,
                                  std::unique_lock<x23::TestLock> const &ul);

  void set_snapshot_data(std::pair<int128_t, ItemLockCacheData> const &snapshot)
  {
    snapshot_cost_ = snapshot.first;

    if (snapshot.second.is_cached_nnint128_base()) {
      snapshot_type_ = kItemLockCacheDataNNInt128BaseValue;
      nnint_base_ = snapshot.second.get_cached_nnint128_base();
    }
    else if (snapshot.second.is_cached_balance()) {
      snapshot_type_ = kItemLockCacheDataBalance;
      auto balance = snapshot.second.get_cached_balance();
      balance_base_ =
          balance.has_value() ? *balance : std::numeric_limits<int128_t>::min();
    }
    else if (snapshot.second.is_rcq()) {
      snapshot_type_ = kItemLockCacheDataRCQ;
    }
    else {
      CHECK(snapshot.second.is_none());
    }
  }

  // Set up lock_contents_ for a type of change.
  // does NOT modify pending_refcount_.
  // Does NOT set the type as ExclusiveLock.
  void prepare_lock_contents(const ItemChange *after, const LockContext &ctx,
                             std::unique_lock<x23::TestLock> const &ul);
  void reset_lock_contents(std::unique_lock<x23::TestLock> const &ul);

  void set_exclusive_lock(std::unique_lock<x23::TestLock> const &ul);

  // No change has committed on this item.
  // As such, its type is not yet known,
  // and the mtx_ must be held to access it.
  constexpr static uint32_t kUncommitted = 0;
  // A change has committed on this item.
  // This means its type is known, and lock_contents_ can be operated on
  // without acquiring the mtx_.
  constexpr static uint32_t kFinalized = 1;
  // Some transaction has committed, but with a permanent exclusive lock.
  // No other transactions can modify the item for the rest of the block.
  constexpr static uint32_t kFinalizedPermamentExclusiveLock = 2;

  std::variant<std::monostate, ExclusiveLock, BytestringLock, NNInt128Lock,
               BalanceLock, DeletionLock, NewContractLock, NewDeploymentLock,
               PointerLock<AuthLock>, PointerLock<ReplayCacheLock>,
               PointerLock<RetrocausalQueueLock>>
      lock_contents_ = std::monostate{};
  // Returns true if the snapshot type has auxiliary keys that need cleanup
  // when the committed type differs. Currently only RCQ qualifies (it has
  // block entries at Prefix::kRetrocausalQueueBlock).
  bool snapshot_has_auxiliary_keys() const
  {
    return snapshot_type_ == kItemLockCacheDataRCQ;
  }

  // snapshot_cost_ and snapshot_type_ are readonly fields: set during the
  // constructor.
  // TODO(geoff): These are only modified during the constructor;
  // some more clever work could make them const.
  //
  // snapshot_type_ records the item's type at block start. It serves two
  // purposes: (1) cache type-specific data (e.g. nnint base value) so we
  // don't have to reload it on each request, and (2) detect type changes
  // that require cleanup of auxiliary keys (e.g. RCQ block entries).

  int128_t snapshot_cost_;

  enum {
    kItemLockCacheDataTypeNone = 0,
    kItemLockCacheDataNNInt128BaseValue = 1,
    kItemLockCacheDataBalance = 2,
    kItemLockCacheDataRCQ = 3,
  };

  union {
    std::monostate padding_none_;
    int128_t nnint_base_;
    int128_t balance_base_;
  };
  uint8_t snapshot_type_ = kItemLockCacheDataTypeNone;

  // This version of the implementation uses the mutex up until the first
  // transaction commits. Concurrent-entry readers must treat kUncommitted as
  // not yet published and return std::nullopt without inspecting
  // lock_contents_. After publication, lock_contents_ no longer changes
  // between variants, and the contents of the variant can be operated on
  // directly.
  std::atomic<uint32_t> lock_state_ = kUncommitted;
  uint32_t pending_refcount_ = 0;
  x23::TestLock lock_;
};

// should be less than two cache lines.
static_assert(sizeof(ItemLock) <= 128);

} // namespace hogd
