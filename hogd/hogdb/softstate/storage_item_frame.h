#pragma once

#include <optional>
#include <set>
#include <stdexcept>

#include "hogd/base/types.h"
#include "hogd/hogdb/softstate/uint256_reservation.h"
#include "hogd/hogdb/storage_item/storage_item_wrapper.h"
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
class StorageItemFrame {
public:
  StorageItemFrame(AddressAndKey const &addrkey,
                   std::optional<StorageItemWrapper> const &snapshot_state);

  // Disable copy
  StorageItemFrame(const StorageItemFrame &) = delete;
  StorageItemFrame &operator=(StorageItemFrame const &) = delete;
  // Allow move
  StorageItemFrame(StorageItemFrame &&) noexcept = default;
  StorageItemFrame &operator=(StorageItemFrame &&obj) noexcept = default;

  // Try to reserve the storage item for the given delta operation.
  // Returns a lock if successful, or nullopt if the reservation failed.
  // The previous_lock parameter represents any existing lock held on this item.
  std::optional<StorageLock> try_reserve(StorageDelta const &delta,
                                         StorageLock const &previous_lock);

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
  void commit(StorageDelta const &delta, StorageLock const &lock);

  // Revert/rollback any changes and release the given lock.
  // The lock must have been obtained from a successful try_reserve() call.
  void revert(StorageLock const &lock);

  // Get the item, before any deltas of the current round were committed.
  // Cannot be called after block close (after get_post_close_block_item() is
  // called).
  std::optional<StorageItemWrapper> const &get_snapshot_item() const;

  // All pending transactions should finish before get_post_close_block_item is
  // called. That is, every try_reserve_delta() is followed by a revert_delta or
  // a commit_delta.
  //
  // This method updates committed_item_ to be the value of the item
  // after the end of the current block.
  std::optional<StorageItemWrapper> const &get_post_close_block_item();

  std::span<const std::byte> get_key() const
  {
    return std::as_bytes(
        std::span(reinterpret_cast<const uint8_t *>(&addr_key_),
                  sizeof(AddressAndKey)));
  }

  // Locking methods
  // upgrade from previous_lock_type to kExclusiveLock or
  // kPermanentExclusiveLock.
  // Caller can decide (externally) whether to make the lock kExclusiveLock or
  // kPermanentExclusiveLock.
  [[nodiscard]] bool
  try_acquire_exclusive_lock(StorageLockType previous_lock_type);

  AddressAndKey const &get_addr_key() const { return addr_key_; }

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

  // Each of the following types represents one possible state that the item
  // may be in, given any pending reservations. Each has a typeclass_match()
  // method, which returns whether the variant can accept a delta of that
  // typeclass.
  //
  // Each also has a to_item method(), which should be called
  // after a block commits(during commit_block()). This converts the
  // committed deltas to a StorageItem for the next block snapshot. All
  // transactions should either revert or commit before to_item() is called.

  // Bytestring state
  struct BytestringArgs {
    Bytestring bytestring_set_;

    bool typeclass_match(Bytestring const &candidate_bytestring) const
    {
      return candidate_bytestring == bytestring_set_;
    }
    std::optional<StorageItemWrapper>
    to_item(std::optional<StorageItemWrapper> const & /*unused*/) const;
  };

  // NNInt state
  struct NNIntSetAddArgs {
    int128_t set_value_;
    // total subtracted by any pending or committed reservations
    int128_t total_subtracted_ = 0;
    UInt256Reservation total_added_;

    bool typeclass_match(int128_t candidate_set_value) const
    {
      return candidate_set_value == set_value_;
    }
    std::optional<StorageItemWrapper>
    to_item(std::optional<StorageItemWrapper> const & /*unused*/) const;
  };

  // Deleted state
  struct DeleteArgs {
    static bool typeclass_match() { return true; }
    static std::optional<StorageItemWrapper>
    to_item(std::optional<StorageItemWrapper> const & /*unused*/)
    {
      return std::nullopt;
    }
  };

  struct RetrocausalQueueArgs {
    // TODO(geoff) contents
    static bool typeclass_match() { return true; }
    std::optional<StorageItemWrapper>
    to_item(std::optional<StorageItemWrapper> const &previous_item);

    // written upon commit.
    std::optional<RetrocausalQueuePastBlockConsumeDelta> past_consume_delta_;
    // Is there an active lock for the consumer position
    bool consumer_lock_ = false;

    // Stores the entries created in a block in a nondeterministic order,
    // but the order has a stable indexing within a block (the first element is
    // always first, etc).
    std::vector<QueueEntry> new_entries_;
    // new_entries_ \setminus consumed entries, in a canonical order
    std::set<QueueEntry> persistable_entries_;
  };

  // Exclusive locked state
  // For convenience, we maintain the invariant that the refcount is 1 whenever
  // the variant is in this state (so i.e. there's never an ExclusiveLockedState
  // with refcount 0).
  struct ExclusiveLockedState {
    static bool typeclass_match() { return false; }
    static std::optional<StorageItemWrapper>
    to_item(std::optional<StorageItemWrapper> const & /*unused*/)
    {
      throw std::runtime_error("Impossible state");
    }
  };

  using variant_t = std::variant<BytestringArgs, NNIntSetAddArgs, DeleteArgs,
                                 RetrocausalQueueArgs, ExclusiveLockedState>;

  // Contents of the variant are ignored if refcount_ is 0.
  // Every successful try_reserve increases the refcount, and revert reduces it.
  class RefcountedVariant {
  public:
    variant_t variant_ = DeleteArgs{};
    uint32_t refcount_ = 0;
    bool permanent_exclusive_lock_ = false;

    RefcountedVariant() = default;

    RefcountedVariant &operator=(RefcountedVariant &&) noexcept = default;
    RefcountedVariant(RefcountedVariant &&) noexcept = default;

    void acquire_reference() { ++refcount_; }

    void release_reference()
    {
      assert(refcount_ > 0);
      --refcount_;
      if (refcount_ == 0) {
        assert(!permanent_exclusive_lock_);
        variant_ = DeleteArgs{};
      }
    }

    // If these methods return true, a reference has been acquired
    bool try_reserve_delete();
    bool try_reserve_bytestring_set(Bytestring const &bytestring);
    bool try_reserve_nnint_set_add(int128_t set_value, int128_t add_value);
    bool try_reserve_retrocausal_queue_delta(bool requires_consume);

    // only valid if refcount_ > 0
    bool matches_typeclass(StorageItemTypeClass typeclass) const;

    // does not change refcount
    // REQUIRES: add_value < 0, and the variant is already in the NNIntSetAdd
    // state
    bool try_reserve_nnint_additional(int128_t add_value);
    // does not change refcount
    // REQUIRES: add_value >= 0, and the variant is already in the NNIntSetAdd
    // state
    void commit_nnint_add(int128_t add_value);

    // does not change refcount
    void revert_nnint_set_add(int128_t add_value);

    // does not change refcount
    bool try_reserve_retrocausal_queue_consume();
    void release_retrocausal_queue_consume();

    // does not change refcount
    void
    commit_retrocausal_queue_modify(RetrocausalQueueDelta const &queue_delta);

  private:
    // Does not manage adjusting the reference count
    template<typename VariantArm, typename... VariantArgs>
    VariantArm *try_set_variant(VariantArgs const &...args);

    template<typename VariantArm> VariantArm &get_variant_arm_unchecked()
    {
      assert(refcount_ > 0);
      return std::get<VariantArm>(variant_);
    }
  };

  void update_committed_item();

  bool round_committed_ = false;
  AddressAndKey addr_key_;
  std::optional<StorageItemWrapper> committed_item_;
  RefcountedVariant variant_;
};

} // namespace hogd
