#pragma once

#include <cassert>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <variant>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/base/wide_integral.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "storage_lock.hh"

namespace hogd {

// Each of the following types represents one possible state that the item
// may be in, given any pending reservations. Each has a typeclass_match()
// method, which returns whether the variant can accept a delta of that
// typeclass.
//
// Each also has a to_item method(), which should be called
// after a block commits(during commit_block()). This converts the
// committed deltas to a StorageItem for the next block snapshot. All
// transactions should either revert or commit before to_item() is called.

// get_current_committed_cost:
//   - returns the storage cost of the current item, including any committed deltas.
//   - Will be equal to to_item(cost_model).storage_cost()

// get_marginal_cost_upper_bound:
//   - returns an upper bound on the *marginal* cost of committing the delta,
//   conditioned on any currently committed deltas

// Bytestring state
struct BytestringArgs {
  Bytestring bytestring_set_;
  bool committed_ = false;

  bool typeclass_match(Bytestring const &candidate_bytestring) const
  {
    return candidate_bytestring == bytestring_set_;
  }

  void commit_set() { committed_ = true; }

  int128_t get_current_committed_cost(StorageCostModel const* current_model, StorageItemWrapper const* /*unused*/) const
  {
    return current_model->kBaseItemCost + current_model->get_bytestring_cost(bytestring_set_.size());
  }

  int128_t get_marginal_cost_upper_bound(StorageDelta const& delta, StorageCostModel const* current_model, StorageItemWrapper const* base_item) const
  {
    // cost for creating the item if it doesn't exist yet
    int128_t base_cost = (base_item || committed_) ? 0 : current_model->kBaseItemCost;
    ssize_t prev_size = (base_item && base_item->type() == StorageItemType::kItemTypeBytestring) ? base_item->bytestring().size() : 0;

    return base_cost + current_model->get_bytestring_cost(delta.bytestring().size() - prev_size);
  }

  std::shared_ptr<const StorageItemWrapper> to_item(StorageCostModel const* cost_model) const;
};

// NNInt state
class NNIntSetAddArgs {
public:
  NNIntSetAddArgs(int128_t set_value)
    : set_value_(set_value), lower_bound_(set_value), upper_bound_(set_value),
      final_value_(set_value)
  {}

  bool typeclass_match(int128_t candidate_set_value) const
  {
    return candidate_set_value == set_value_;
  }

  constexpr static int128_t get_current_committed_cost(StorageCostModel const* current_model, StorageItemWrapper const* /*unused*/)
  {
    return current_model->kBaseItemCost;
  }

  int128_t get_marginal_cost_upper_bound(StorageDelta const& delta, StorageCostModel const* current_model, StorageItemWrapper const* base_item) const
  {
    return (committed_ || base_item) ? 0 : current_model->kBaseItemCost;
  }

  std::shared_ptr<const StorageItemWrapper> to_item(StorageCostModel const* cost_model) const;

  void release_bounds_lock(NNIntBoundsLock const &bounds_lock);
  bool try_acquire_bounds_lock(NNIntBoundsLock const &bounds_lock);

  // add can be positive or negative
  void commit_add(int128_t add_value);

  bool in_valid_state() const;

private:
  int128_t set_value_;
  bool committed_ = false;

  using bounds_t = WideIntegral<int128_t>;
  bounds_t lower_bound_, upper_bound_, final_value_;
};

// Deleted state
struct DeleteArgs {
  static bool typeclass_match() { return true; }
  static std::shared_ptr<const StorageItemWrapper> to_item() { return nullptr; }
  constexpr static int128_t get_current_committed_cost(StorageCostModel const* /*unused*/, StorageItemWrapper const* /*unused*/) { return 0; }
  constexpr static int128_t get_marginal_cost_upper_bound(StorageDelta const& /*unused*/, StorageCostModel const* /*unused*/, StorageItemWrapper const* /*unused*/) { return 0; }
};

// Retrocausal queue state
class RetrocausalQueueArgs {
public:
  static bool typeclass_match() { return true; }
  std::shared_ptr<const StorageItemWrapper>
  to_item(const StorageItemWrapper *previous_item, StorageCostModel const* cost_model) const;

  int128_t get_cost(RetrocausalQueueDelta const &delta,
                    const StorageItemWrapper *base_item) const;

  int128_t get_current_committed_cost(StorageCostModel const* current_model, StorageItemWrapper const* base_item) const;
  int128_t get_marginal_cost_upper_bound(StorageDelta const& delta, StorageCostModel const* current_model, StorageItemWrapper const* base_item) const;

  bool try_reserve_consume()
  {
    if (past_consume_delta_) {
      return false;
    }
    if (consumer_lock_) {
      return false;
    }
    consumer_lock_ = true;
    return true;
  }

  void commit_consume(RetrocausalQueueConsumeDelta const &consume_delta,
                      InvalidStateFlag &invalid_state_flag);

  void revert_consume()
  {
    assert(consumer_lock_);
    assert(!past_consume_delta_);
    consumer_lock_ = false;
  }

  void commit_insert(RetrocausalQueueInsertDelta const &insert_delta,
                     InvalidStateFlag &invalid_state_flag);

  std::optional<QueueEntry>
  get_concurrent_retrocausal_queue_entry(uint32_t index) const;

private:
  RetrocausalQueueStats get_current_committed_stats(StorageItemWrapper const* base_item) const;

  // written upon commit.
  std::optional<RetrocausalQueuePastBlockConsumeDelta> past_consume_delta_;
  // Is there an active lock for the consumer position
  bool consumer_lock_ = false;
  // If an insert commits, whether or not the block is fully retrocausally
  // consumed, we need to bump the block number. This greatly simplifies the
  // accounting logic for storage costs.
  bool insert_committed_ = false;

  // Stores the entries created in a block in a nondeterministic order,
  // but the order has a stable indexing within a block (the first element is
  // always first, etc).
  std::vector<QueueEntry> new_entries_;
  // new_entries_ \setminus consumed entries, in a canonical order
  std::multiset<QueueEntry> persistable_entries_;
  // During validation, we might try to consume entries before the
  // corresponding insert transaction executes. At the end of the block, we
  // assert that this set is empty.
  std::multiset<QueueEntry> consumed_but_not_inserted_entries_;

  static constexpr RetrocausalQueuePastBlockConsumeDelta kEmptyConsume{
      .max_fully_consumed_block_number = 0, .partial_consumed_number = 0};
};

// Exclusive locked state
// For convenience, we maintain the invariant that the refcount is 1 whenever
// the variant is in this state (so i.e. there's never an ExclusiveLockedState
// with refcount 0).
struct ExclusiveLockedState {
  static bool typeclass_match() { return false; }
  static std::shared_ptr<const StorageItemWrapper> to_item()
  {
    throw std::runtime_error("Impossible state");
  }
  static int128_t get_current_committed_cost(StorageCostModel const* cost_model, StorageItemWrapper const* base_item) { 
    return base_item->storage_cost(cost_model);
  }

  static int128_t get_marginal_cost_upper_bound(StorageDelta const& delta, StorageCostModel const* cost_model, StorageItemWrapper const* base_item)
  {
    // TODO(geoff): Most of these methods could be factored out into a static and nonstatic version
    switch (delta.type()) {
      case StorageDeltaType::kDeltaTypeBytestringSet:
      {
        BytestringArgs args;
        return args.get_marginal_cost_upper_bound(delta, cost_model, base_item);
      }
      case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
      {
        NNIntSetAddArgs args(from_xdr(delta.nnint().set_value));
        return args.get_marginal_cost_upper_bound(delta, cost_model, base_item);
      }
      case StorageDeltaType::kDeltaTypeDelete:
        return DeleteArgs::get_marginal_cost_upper_bound(delta, cost_model, base_item);
      case StorageDeltaType::kDeltaTypeRetrocausalQueue:
      {
        RetrocausalQueueArgs args;
        return args.get_marginal_cost_upper_bound(delta, cost_model, base_item);
      }
      default:
        throw std::runtime_error("Impossible state");
    }
  }
};

// Unmodified state -- the variant is in this state if and only if refcount is
// 0.
struct UnmodifiedState {
  static bool typeclass_match() { return true; }
  static std::shared_ptr<const StorageItemWrapper>
  to_item(std::shared_ptr<const StorageItemWrapper> item)
  {
    return item;
  }

  static int128_t get_current_committed_cost(StorageCostModel const* /*unused*/, StorageItemWrapper const* base_item) { 
    // base item stores the cost model under which it was added to the snapshot
    return base_item->storage_cost();
  }

  static int128_t get_marginal_cost_upper_bound(StorageDelta const& /*unused*/, StorageCostModel const* /*unused*/, StorageItemWrapper const* /*unused*/)
  {
    // refcount must be nonzero to call get_cost_upper_bound
    throw std::runtime_error("Impossible state");
  }
};

// RefcountedDeltaVariant manages the state of a storage item that can be
// modified by multiple concurrent transactions. It uses a reference count to
// track active reservations and maintains variant state based on the type of
// modifications being made.
//
// The variant can be in one of several states:
// - BytestringArgs: For bytestring storage items that can be set to specific
// values
// - NNIntSetAddArgs: For non-negative integer items that support atomic add
// operations
// - RetrocausalQueueArgs: For retrocausal queue items that support
// insert/consume operations
// - ExclusiveLockedState: When an item is exclusively locked by a transaction.
// Note that a transaction can put the variant in this state in two ways; one,
// by performing a storage operation with a kExclusiveLock flag set, and two, by
// performing multiple reservations of different typeclasses on the same item
// (via e.g. try_invoke()).
// - DeleteArgs: When an item is marked for deletion
// - UnmodifiedState: The default state when no transactions are modifying the
// item
//
// The refcount tracks how many transactions have active reservations. When it
// drops to 0, the variant reverts to UnmodifiedState. Permanent exclusive locks
// can be acquired to prevent other transactions from modifying the item.
//
// Key operations:
// - try_set_variant(): Attempts to set the variant
// state.  Acquires a reference if successful.
// - release_reference(): Releases a reference, potentially reverting to
// UnmodifiedState
// - get_cost_after_commit(): Calculates storage costs after a delta is
// committed
// - matches_typeclass(): Checks if current state matches a storage item type
// - to_item(): Converts variant state into a StorageItemWrapper, to be passed
// to the next block.
class RefcountedDeltaVariant {
  using variant_t =
      std::variant<BytestringArgs, NNIntSetAddArgs, RetrocausalQueueArgs,
                   ExclusiveLockedState, DeleteArgs, UnmodifiedState>;

public:
  RefcountedDeltaVariant() = default;

  RefcountedDeltaVariant &
  operator=(RefcountedDeltaVariant &&) noexcept = default;
  RefcountedDeltaVariant(RefcountedDeltaVariant &&) noexcept = default;

  void release_reference()
  {
    assert(refcount_ > 0);
    --refcount_;
    if (refcount_ == 0) {
      assert(!permanent_exclusive_lock_);
      variant_ = UnmodifiedState{};
    }
  }

  // Get the cost of storing the current state of the item,
  // counting _only_ the currently committed deltas.
  int128_t get_current_committed_cost(StorageCostModel const* current_model, StorageItemWrapper const* base_item) const;

  // Compute an upper bound on the amount of costs
  // that must be locked, if the delta is committed.
  // This is an upper bound, not strict equality.
  // Accounts for potential changes in the storage cost model.
  int128_t get_marginal_cost_upper_bound(
    StorageDelta const& delta, 
    StorageCostModel const* current_model, 
    StorageItemWrapper const* base_item) const;

  // Acquires a reference to the variant
  // Returns nullptr if setting the variant fails.
  template<typename VariantArm, typename... VariantArgs>
  VariantArm *try_set_variant(VariantArgs const &...args)
  {
    if (refcount_ == 0) {
      assert(std::holds_alternative<UnmodifiedState>(variant_));
      variant_.emplace<VariantArm>(args...);
      refcount_++;
      return &std::get<VariantArm>(variant_);
    }

    if (!std::holds_alternative<VariantArm>(variant_)) {
      return nullptr;
    }

    if (!std::get<VariantArm>(variant_).typeclass_match(args...)) {
      return nullptr;
    }

    refcount_++;
    return &std::get<VariantArm>(variant_);
  }

  // Must already have a reference to the variant for the expected type.
  // Does not acquire a reference.
  template<typename VariantArm> auto &get_variant_arm_unchecked(this auto &self)
  {
    assert(self.refcount_ > 0);
    assert(std::holds_alternative<VariantArm>(self.variant_));
    return std::get<VariantArm>(self.variant_);
  }

  template<typename VariantArm> bool has_variant_arm() const
  {
    return std::holds_alternative<VariantArm>(variant_);
  }

  // Returns true if the current state matches the given typeclass.
  // Requires: refcount_ > 0
  bool matches_typeclass(StorageItemTypeClass typeclass) const;

  void set_permanent_exclusive_lock() { permanent_exclusive_lock_ = true; }
  bool has_permanent_exclusive_lock() const
  {
    return permanent_exclusive_lock_;
  }

  uint32_t refcount() const { return refcount_; }

  // Converts the current state into a StorageItemWrapper, to be passed to the
  // next block.
  std::shared_ptr<const StorageItemWrapper>
  to_item(std::shared_ptr<const StorageItemWrapper> previous_item, StorageCostModel const* cost_model) const;

private:
  variant_t variant_ = UnmodifiedState{};
  uint32_t refcount_ = 0;
  bool permanent_exclusive_lock_ = false;
};

} // namespace hogd
