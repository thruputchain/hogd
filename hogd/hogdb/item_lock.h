#pragma once

#include <atomic>
#include <mutex>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/auth_lock.h"
#include "hogd/hogdb/item_lock/bytestring_lock.h"
#include "hogd/hogdb/item_lock/deletion_lock.h"
#include "hogd/hogdb/item_lock/exclusive_lock.h"
#include "hogd/hogdb/item_lock/new_contract_lock.h"
#include "hogd/hogdb/item_lock/new_deployment_lock.h"
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/testlock.h"

namespace hogd {

class ItemLock {
public:
  ItemLock(int128_t snapshot_cost) : snapshot_cost_(snapshot_cost) {}

  // If after!=nullptr, returns a deterministic upper bound on the storage cost
  // required to support the after change. Is a no-op on failure.
  HogDBStatus<int128_t>
  try_atomic_change_lock(const ItemChange *before, const ItemChange *after,
                         const StorageCostModel *cost_model);

  // Commits the change to the item lock.
  // Returns the storage cost used to commit the change.
  // Must be less than or equal to the storage cost returned by
  // try_atomic_change_lock for that change.
  // change must be non-null.
  int128_t commit_change(const ItemChange &change,
                         const StorageCostModel *cost_model);

  HogDBStatus<void> write_committed_change(ItemKey const &key, HogDBDAO &dao,
                                           MerkleTriePrefix &trie) const;

  bool is_finalized() const { return lock_state_ != kUncommitted; }

private:
  HogDBStatus<int128_t>
  handle_try_atomic_from_finalized(const ItemChange *before,
                                   const ItemChange *after,
                                   const StorageCostModel *cost_model);
  HogDBStatus<int128_t>
  handle_try_atomic_from_pending(const ItemChange *before,
                                 const ItemChange *after,
                                 const StorageCostModel *cost_model,
                                 std::unique_lock<x23::TestLock> const &ul);

  // No change has committed on this item, and we would like to try to acquire a
  // change in a transaction that does not currently hold any lock on this item.
  HogDBStatus<int128_t>
  handle_uncommitted_try_new_lock(const ItemChange *after,
                                  const StorageCostModel *cost_model,
                                  std::unique_lock<x23::TestLock> &ul);
  void
  handle_release_uncommitted_lock(const ItemChange *before,
                                  const StorageCostModel *cost_model,
                                  std::unique_lock<x23::TestLock> const &ul);

  // Set up lock_contents_ for a type of change.
  // does NOT modify pending_refcount_.
  // Does NOT set the type as ExclusiveLock.
  void prepare_lock_contents(const ItemChange *after,
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
               DeletionLock, NewContractLock, NewDeploymentLock, AuthLock>
      lock_contents_ = std::monostate{};

  std::atomic<uint32_t> lock_state_ = kUncommitted;

  // This version of the implementation uses the mutex up until the first
  // transaction commits. After that point, lock_contents_ no longer changes
  // between variants, and the contents of the variant can be operated on
  // directly.

  uint32_t pending_refcount_ = 0;

  int128_t snapshot_cost_;
  x23::TestLock lock_;
};

// should be less than two cache lines.
// Note: AuthLock is large (contains full AccountAuthData and pk_changes array),
// so ItemLock exceeds the 128-byte limit. User explicitly said not to worry
// about size. static_assert(sizeof(ItemLock) <= 128);

} // namespace hogd
