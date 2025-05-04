#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>

#include "hogd/base/types.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/refcounted_delta_variant.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "storage_lock.hh"

namespace hogd {

// Manage concurrent accesses to a StorageItem in the database.
//
// Some data types will require internal data structures more complex than a
// StorageItem,(which is really a serialization format more than an efficient
// data structure). The internals may need reimplementation when we have a
// database to interact with. However, the external interface should look
// approximately like this.
//
// This structure is NOT threadsafe in itself.  We assume that this structure
// will be guarded by some external lock.
class StorageItemDeltaManager {
public:
  StorageItemDeltaManager(
      std::shared_ptr<const StorageItemWrapper> snapshot_state,
      InvalidStateFlag *invalid_state_flag);

  // Disable copy
  StorageItemDeltaManager(const StorageItemDeltaManager &) = delete;
  StorageItemDeltaManager &operator=(StorageItemDeltaManager const &) = delete;
  // Allow move
  StorageItemDeltaManager(StorageItemDeltaManager &&) noexcept = default;
  StorageItemDeltaManager &
  operator=(StorageItemDeltaManager &&obj) noexcept = default;

  // Try to reserve the storage item for the given delta operation.
  // Returns a lock if successful, or nullopt if the reservation failed.
  // The previous_lock parameter represents any existing lock held on this item.
  std::optional<MeteredStorageLock>
  try_reserve(StorageDelta const &delta, StorageLock const &previous_lock);

  // TODO(geoff): If we so desire (which we probably do? Unclear around DoS risk
  // for contracts, will investigate) we may want to expose an interface of the
  // form "release_lock" or "pop_lock", which would release a held lock,
  // exchanging with a previously held (weaker) lock. That would require
  // maintenance of a ''lock stack'' in the TransactionCache, and a fair amount
  // of nontrivial (possibly bugprone) logic in the executor. We may add that
  // later, but for now it is not crucial.

  // Commit the delta operation under the given lock.
  // This releases the lock and applies the changes from the delta.
  // The lock must have been obtained from a successful try_reserve() call.
  int128_t commit(StorageDelta const &delta, MeteredStorageLock const &lock);

  // Revert/rollback any changes and release the given lock.
  // The lock must have been obtained from a successful try_reserve() call.
  void revert(StorageLock const &lock);

  // Get the item, before any deltas of the current round were committed.
  // Cannot be called after block close (after get_post_close_block_item() is
  // called).
  std::shared_ptr<const StorageItemWrapper> get_snapshot_item() const;

  // All pending transactions should finish before get_post_close_block_item is
  // called. That is, every try_reserve_delta() is followed by a revert_delta or
  // a commit_delta.
  //
  // This method updates next_round_item_ to be the value of the item
  // after the end of the current block.
  std::shared_ptr<const StorageItemWrapper> get_post_close_block_item();

  // Locking methods
  // upgrade from previous_lock_type to kExclusiveLock or
  // kPermanentExclusiveLock.
  [[nodiscard]] std::optional<MeteredStorageLock>
  try_acquire_exclusive_lock(StorageDelta const &delta,
                             StorageLockType previous_lock_type,
                             StorageLockType desired_lock_type);

  // WARNING: nondeterministic
  std::optional<QueueEntry>
  get_concurrent_retrocausal_queue_entry(uint32_t index);

private:
  // Performs a reservation of the storage item for the given delta operation.
  // Should be performed only when the initial lock state is kNone.
  //
  // Returns true if reservation is successful.
  //
  // TODO(geoff): We can probably just remove the commit_delta() method
  // entirely.  Will wait to remove until we
  // have all the other StorageDelta types worked out.
  [[nodiscard]] bool try_reserve_delta(StorageDelta const &delta);

  void commit_delta(StorageDelta const &delta,
                    bool maintain_permanent_exclusive_lock);

  // Locking methods
  // kExclusiveLock -> kNone
  void revert_exclusive_lock_to_none();
  // kExclusiveLock -> kSetItemTypeClass
  void revert_exclusive_lock_to_set_typeclass(StorageDelta const &delta);
  // kSetItemTypeClass -> kSetItemTypeClass
  void revert_set_typeclass_lock_to_set_typeclass(StorageLock const &lock,
                                                  StorageDelta const &delta);
  // kSetItemTypeClass -> kNone
  void revert_set_typeclass_lock(TypeSpecificLock const &type_specific_lock);

  // kSetItemTypeClass -> kSetItemTypeClass
  // Requires: previous lock is a set_typeclass lock, and reserving delta
  // does not require upgrading to an exclusive lock.
  std::optional<StorageLock>
  handle_try_reserve_from_set_typeclass_lock(StorageDelta const &delta,
                                             StorageLock const &prev_lock);

  void produce_next_round_item();

  bool round_committed_ = false;
  std::shared_ptr<const StorageItemWrapper> committed_item_;
  std::shared_ptr<const StorageItemWrapper> next_round_item_ = nullptr;

  int128_t committed_item_cost_; // committed_item_->get_locked_cost()
  int128_t unreleased_storage_cost_ = 0;

  RefcountedDeltaVariant variant_;
  InvalidStateFlag *invalid_state_flag_;
};

} // namespace hogd
