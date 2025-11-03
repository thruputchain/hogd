#pragma once

#include <atomic>
#include <mutex>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/bytestring_lock.h"
#include "hogd/hogdb/item_lock/exclusive_lock.h"
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

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
  void commit_change(const ItemChange *before,
                     const StorageCostModel *cost_model);

  // Returns the storage cost of the item, after any committed changes.
  // returns the snapshot cost if no changes have been committed.
  int128_t committed_storage_cost(const StorageCostModel *cost_model) const;

private:
  HogDBStatus<int128_t>
  handle_try_atomic_from_finalized(const ItemChange *before,
                                   const ItemChange *after,
                                   const StorageCostModel *cost_model);
  HogDBStatus<int128_t>
  handle_try_atomic_from_pending(const ItemChange *before,
                                 const ItemChange *after,
                                 const StorageCostModel *cost_model,
                                 std::unique_lock<std::mutex> const &ul);

  // No change has committed on this item, and we would like to try to acquire a
  // change in a transaction that does not currently hold any lock on this item.
  HogDBStatus<int128_t>
  handle_uncommitted_try_new_lock(const ItemChange *after,
                                  const StorageCostModel *cost_model,
                                  std::unique_lock<std::mutex> &ul);
  void handle_release_uncommitted_lock(const ItemChange *before,
                                       const StorageCostModel *cost_model,
                                       std::unique_lock<std::mutex> const &ul);

  // Set up lock_contents_ for a type of change.
  // does NOT modify pending_refcount_.
  void prepare_lock_contents(const ItemChange *after,
                             std::unique_lock<std::mutex> const &ul);
  void reset_lock_contents(std::unique_lock<std::mutex> const &ul);

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

  std::atomic<uint32_t> lock_state_ = kUncommitted;

  // This version of the implementation uses the mutex up until the first
  // transaction commits. After that point, lock_contents_ no longer changes
  // between variants, and the contents of the variant can be operated on
  // directly.
  std::mutex mtx_;
  uint32_t pending_refcount_ = 0;
  std::variant<std::monostate, ExclusiveLock, BytestringLock, NNInt128Lock>
      lock_contents_ = std::monostate{};

  int128_t snapshot_cost_;
};

} // namespace hogd
