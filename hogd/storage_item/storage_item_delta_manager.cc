#include "hogd/storage_item/storage_item_delta_manager.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/refcounted_delta_variant.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"

namespace hogd {

StorageItemDeltaManager::StorageItemDeltaManager(
    std::shared_ptr<const StorageItemWrapper> snapshot_state,
    InvalidStateFlag *invalid_state_flag,
    BlockExecutionMode execution_mode)
  : committed_item_(std::move(snapshot_state)),
    committed_item_cost_(committed_item_ ? committed_item_->storage_cost() : 0),
    invalid_state_flag_(invalid_state_flag),
    execution_mode_(execution_mode)
{}

bool
StorageItemDeltaManager::try_reserve_delta(StorageDelta const &delta)
{
  assert(!variant_.has_permanent_exclusive_lock());

  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    return variant_.try_set_variant<BytestringArgs>(delta.bytestring()) !=
           nullptr;
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
    if (auto *args = variant_.try_set_variant<NNIntSetAddArgs>(
            from_xdr(delta.nnint().set_value))) {
      int128_t add_value = from_xdr(delta.nnint().add_value);
      int128_t set_value = from_xdr(delta.nnint().set_value);
      if (add_value >= 0) {
        // modifying total_added_ is deferred to commit time
        return true;
      }

      // if set_value <=0 and add_value < 0, delta always fails
      if (set_value <= 0) {
        variant_.release_reference();
        return false;
      }

      // the interesting case;
      // set_value > 0, add_value < 0
      auto res = args->try_reserve_add(add_value);
      if (!res) {
        variant_.release_reference();
        return false;
      }
      return true;
    }
    return false;
  case StorageDeltaType::kDeltaTypeDelete:
    return variant_.try_set_variant<DeleteArgs>() != nullptr;
  case StorageDeltaType::kDeltaTypeRetrocausalQueue:
  {
    bool requires_consume = delta.retrocausal_queue().consume != nullptr;
    if (auto *args = variant_.try_set_variant<RetrocausalQueueArgs>()) {
      if (requires_consume) {
        return args->try_reserve_consume();
      }
      return true;
    }
    return false;
  }
  default:
    throw std::runtime_error("unknown delta type");
  }
}

void
StorageItemDeltaManager::commit_delta(StorageDelta const &delta,
                                      bool maintain_permanent_exclusive_lock)
{
  assert(variant_.refcount() > 0);
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    break;
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
  {
    auto &args = variant_.get_variant_arm_unchecked<NNIntSetAddArgs>();
    int128_t add_value = from_xdr(delta.nnint().add_value);
    if (add_value > 0) {
      args.commit_add(add_value);
    }
  } break;
  case StorageDeltaType::kDeltaTypeDelete:
    break;
  case StorageDeltaType::kDeltaTypeRetrocausalQueue:
  {
    auto &args = variant_.get_variant_arm_unchecked<RetrocausalQueueArgs>();
    args.commit_insert(delta.retrocausal_queue().insert, *invalid_state_flag_);
    if (delta.retrocausal_queue().consume) {
      args.commit_consume(*delta.retrocausal_queue().consume,
                          *invalid_state_flag_);
    }
  } break;
  default:
    throw std::runtime_error("unimpl");
  }

  if (maintain_permanent_exclusive_lock) {
    variant_.set_permanent_exclusive_lock();
  }
}

std::shared_ptr<const StorageItemWrapper>
StorageItemDeltaManager::get_snapshot_item() const
{
  assert(!round_committed_);
  return committed_item_;
}

std::shared_ptr<const StorageItemWrapper>
StorageItemDeltaManager::get_post_close_block_item()
{
  produce_next_round_item();
  assert(round_committed_);
  return next_round_item_;
}

void
StorageItemDeltaManager::produce_next_round_item()
{
  if (!round_committed_) {
    next_round_item_ = variant_.to_item(committed_item_);
    round_committed_ = true;
  }
}
std::optional<MeteredStorageLock>
StorageItemDeltaManager::try_acquire_exclusive_lock(
    StorageDelta const &delta, StorageLockType previous_lock_type,
    StorageLockType desired_lock_type)
{
  assert(!round_committed_);
  assert(desired_lock_type == StorageLockType::kExclusiveLock ||
         desired_lock_type == StorageLockType::kPermanentExclusiveLock);

  if (variant_.has_permanent_exclusive_lock()) {
    assert(variant_.refcount() > 0);
    return std::nullopt;
  }

  if (variant_.refcount() == 0) {
    assert(previous_lock_type == StorageLockType::kNone);
    [[maybe_unused]] auto *res =
        variant_.try_set_variant<ExclusiveLockedState>();
    assert(res);

    auto required_cost =
        variant_.get_cost_after_commit(delta, committed_item_.get());

    return MeteredStorageLock{
        .lock_ = StorageLock(desired_lock_type),
        .locked_tokens_ =
            std::max<int128_t>(0, required_cost - unreleased_storage_cost_ -
                                      committed_item_cost_),
    };
  }

  switch (previous_lock_type) {
  case StorageLockType::kNone:
  {
    assert(variant_.refcount() > 0); // refcount_ == 0 case handled above
    return std::nullopt;
  }
  case StorageLockType::kSetItemTypeClass:
  {
    // do we have the only exclusive lock?
    bool res = (variant_.refcount() == 1);
    // If we have a SetTypeClass lock, then nobody can have an exclusive lock or
    // a permanent exclusive lock
    assert(!variant_.has_permanent_exclusive_lock());
    assert(!variant_.has_variant_arm<ExclusiveLockedState>());

    if (!res) {
      // we don't have the only reference, so we can't upgrade to exclusive
      return std::nullopt;
    }

    // release the reference we had, and then acquire one for exclusive state
    variant_.release_reference();
    [[maybe_unused]] auto *r = variant_.try_set_variant<ExclusiveLockedState>();
    assert(r);

    auto required_cost =
        variant_.get_cost_after_commit(delta, committed_item_.get());

    return MeteredStorageLock{
        .lock_ = StorageLock(desired_lock_type),
        .locked_tokens_ =
            std::max<int128_t>(0, required_cost - unreleased_storage_cost_ -
                                      committed_item_cost_),
    };
  }
  case StorageLockType::kExclusiveLock:
  case StorageLockType::kPermanentExclusiveLock:
    // we already have the exclusive lock, so we can return true
    // This technically seems like acquiring the lock that we already have
    // but this lets the external logic be agnostic to whether we already have
    // the lock or not.
    auto required_cost =
        variant_.get_cost_after_commit(delta, committed_item_.get());

    return MeteredStorageLock{
        .lock_ = StorageLock(desired_lock_type),
        .locked_tokens_ =
            std::max<int128_t>(0, required_cost - unreleased_storage_cost_ -
                                      committed_item_cost_),
    };
  }

  assert(false);
  throw std::runtime_error("unreachable");
}

void
StorageItemDeltaManager::revert_exclusive_lock_to_none()
{
  variant_.release_reference();
  assert(variant_.refcount() == 0);
}

void
StorageItemDeltaManager::revert_exclusive_lock_to_set_typeclass(
    StorageDelta const &delta)
{
  assert(variant_.refcount() == 1);
  variant_.release_reference();
  // refcount is 0, try_reserve_delta thinks this is an unmodified item now
  [[maybe_unused]]
  bool res = try_reserve_delta(delta); // reacquires a reference
  assert(res);
  assert(variant_.refcount() == 1);
}

void
StorageItemDeltaManager::revert_set_typeclass_lock(
    TypeSpecificLock const &type_specific_lock)
{
  switch (type_specific_lock.item_type()) {
  case StorageItemType::kItemTypeNonnegativeInt:
  {
    int128_t sub_amount =
        from_xdr(type_specific_lock.nnint_subtracted_amount());
    assert(sub_amount <= 0);
    auto &args = variant_.get_variant_arm_unchecked<NNIntSetAddArgs>();
    if (sub_amount < 0) {
      args.revert_add(sub_amount);
    }
    break;
  }
  case StorageItemType::kItemTypeRetrocausalQueue:
  {
    auto &args = variant_.get_variant_arm_unchecked<RetrocausalQueueArgs>();
    if (type_specific_lock.consume_lock() == 1) {
      args.revert_consume();
    }
    break;
  }
  default:
    break;
  }

  variant_.release_reference();
}

std::optional<StorageLock>
StorageItemDeltaManager::handle_try_reserve_from_set_typeclass_lock(
    StorageDelta const &delta, StorageLock const &prev_lock)
{

  auto const &prev_typeclass_lock = prev_lock.full_typeclass_lock();
  assert(variant_.refcount() > 0);
  assert(!variant_.has_permanent_exclusive_lock());

  auto req_lock = delta_to_lock(delta);
  assert(req_lock.lock_type() == StorageLockType::kSetItemTypeClass);

  if (req_lock.full_typeclass_lock().typeclass !=
      prev_typeclass_lock.typeclass) {
    // upgrade to exclusive
    auto lk =
        try_acquire_exclusive_lock(delta, StorageLockType::kSetItemTypeClass,
                                   StorageLockType::kExclusiveLock);
    if (lk) {
      return lk->lock_;
    }
    return std::nullopt;
  }

  assert(variant_.matches_typeclass(prev_typeclass_lock.typeclass));
  assert(variant_.matches_typeclass(req_lock.full_typeclass_lock().typeclass));

  switch (req_lock.full_typeclass_lock().extra_lock_data.item_type()) {
  case StorageItemType::kItemTypeNonnegativeInt:
  {
    int128_t prev_sub_amount =
        from_xdr(prev_typeclass_lock.extra_lock_data.nnint_subtracted_amount());
    int128_t req_sub_amount =
        from_xdr(req_lock.full_typeclass_lock()
                     .extra_lock_data.nnint_subtracted_amount());

    assert(prev_sub_amount <= 0);
    assert(req_sub_amount <= 0);
    assert(prev_sub_amount != std::numeric_limits<int128_t>::min());
    if (prev_sub_amount <= req_sub_amount) {
      return prev_lock;
    }

    assert(variant_.has_variant_arm<NNIntSetAddArgs>());

    auto &args = variant_.get_variant_arm_unchecked<NNIntSetAddArgs>();

    auto reserve_res = args.try_reserve_add(req_sub_amount - prev_sub_amount);
    if (!reserve_res) {
      return std::nullopt;
    }

    return req_lock;
  } break;
  case StorageItemType::kItemTypeRetrocausalQueue:
  {
    assert(variant_.has_variant_arm<RetrocausalQueueArgs>());
    auto &args = variant_.get_variant_arm_unchecked<RetrocausalQueueArgs>();

    if (req_lock.full_typeclass_lock().extra_lock_data.consume_lock() == 1) {
      if (args.try_reserve_consume()) {
        return req_lock;
      }
      return std::nullopt;
    }
    assert(req_lock.full_typeclass_lock() == prev_typeclass_lock);
    return req_lock;
  }
  default:
    return prev_lock;
  }
}

std::optional<MeteredStorageLock>
StorageItemDeltaManager::try_reserve(StorageDelta const &delta,
                                     StorageLock const &previous_lock)
{
  assert(!round_committed_);
  switch (previous_lock.lock_type()) {
  case StorageLockType::kNone:
  {
    if (variant_.has_permanent_exclusive_lock()) {
      return std::nullopt;
    }
    if (try_reserve_delta(delta)) {

      int128_t required_storage =
          variant_.get_cost_after_commit(delta, committed_item_.get()) -
          unreleased_storage_cost_ - committed_item_cost_;
      return MeteredStorageLock{.lock_ = delta_to_lock(delta),
                                .locked_tokens_ =
                                    std::max<int128_t>(required_storage, 0)};
    }
    return std::nullopt;
  }
  case StorageLockType::kSetItemTypeClass:
  {
    auto out = handle_try_reserve_from_set_typeclass_lock(delta, previous_lock);
    if (!out) {
      return std::nullopt;
    }

    int128_t required_storage =
        variant_.get_cost_after_commit(delta, committed_item_.get()) -
        unreleased_storage_cost_ - committed_item_cost_;

    return MeteredStorageLock{.lock_ = *out,
                              .locked_tokens_ =
                                  std::max<int128_t>(required_storage, 0)};
  }
  // always succeeds if we have an exclusive lock
  case StorageLockType::kExclusiveLock:
  case StorageLockType::kPermanentExclusiveLock:
  {
    auto required_cost =
        variant_.get_cost_after_commit(delta, committed_item_.get()) -
        unreleased_storage_cost_ - committed_item_cost_;

    return MeteredStorageLock{.lock_ = previous_lock,
                              .locked_tokens_ =
                                  std::max<int128_t>(required_cost, 0)};
  }
  default:
    throw std::runtime_error("impossible");
  }
}

void
StorageItemDeltaManager::revert_set_typeclass_lock_to_set_typeclass(
    StorageLock const &lock, StorageDelta const &delta)
{
  assert(lock.lock_type() == StorageLockType::kSetItemTypeClass);
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    // nothing to do
    break;
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
  {
    assert(lock.full_typeclass_lock().extra_lock_data.item_type() ==
           StorageItemType::kItemTypeNonnegativeInt);
    assert(delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);

    int128_t actual_sub_amount =
        std::min<int128_t>(0, from_xdr(delta.nnint().add_value));
    int128_t locked_sub_amount = from_xdr(
        lock.full_typeclass_lock().extra_lock_data.nnint_subtracted_amount());

    assert(actual_sub_amount <= 0);
    assert(locked_sub_amount <= 0);
    assert(actual_sub_amount >= locked_sub_amount);
    // so locked_sub_amount - actual_sub_amount is negative
    assert(locked_sub_amount - actual_sub_amount <= 0);

    int128_t diff = locked_sub_amount - actual_sub_amount;
    if (diff < 0) {
      auto &args = variant_.get_variant_arm_unchecked<NNIntSetAddArgs>();
      args.revert_add(diff);
    }
    break;
  }
  case StorageDeltaType::kDeltaTypeDelete:
    // nothing to do
    break;
  case StorageDeltaType::kDeltaTypeRetrocausalQueue:
  {
    // If we took out the consume lock, but no longer need it
    if (lock.full_typeclass_lock().extra_lock_data.consume_lock() == 1) {
      if (!delta.retrocausal_queue().consume) {
        variant_.get_variant_arm_unchecked<RetrocausalQueueArgs>()
            .revert_consume();
      }
    }
    break;
  }
  default:
    throw std::runtime_error("unimplemented");
  }
}

int128_t
StorageItemDeltaManager::commit(StorageDelta const &delta,
                                MeteredStorageLock const &lock)
{
  assert(!round_committed_);
  assert(lock.lock_.lock_type() != StorageLockType::kNone);

  // We want to compute required_storage after releasing any intermediate locks
  // held (which might affect costs, as in the case of kExclusiveLock)
  // but before we actually apply the commitment, to be consistent
  // with the try_reserve usage.
  int128_t required_storage = 0;

  switch (lock.lock_.lock_type()) {
  case StorageLockType::kSetItemTypeClass:
    revert_set_typeclass_lock_to_set_typeclass(lock.lock_, delta);
    required_storage =
        variant_.get_cost_after_commit(delta, committed_item_.get());
    commit_delta(delta, false);
    break;
  case StorageLockType::kExclusiveLock:
    revert_exclusive_lock_to_set_typeclass(delta);
    required_storage =
        variant_.get_cost_after_commit(delta, committed_item_.get());
    commit_delta(delta, false);
    break;
  case StorageLockType::kPermanentExclusiveLock:
    revert_exclusive_lock_to_set_typeclass(delta);
    required_storage =
        variant_.get_cost_after_commit(delta, committed_item_.get());
    commit_delta(delta, true);
    break;
  case StorageLockType::kNone:
    assert(false);
  default:
    throw std::runtime_error("impossible");
  }
  assert(required_storage >= 0);

  // want: required_storage = base_storage + unreleased_storage_cost_.
  int128_t locked_storage = lock.locked_tokens_;
  assert(committed_item_cost_ + unreleased_storage_cost_ + locked_storage >=
         required_storage);

  int128_t storage_excess = committed_item_cost_ + unreleased_storage_cost_ +
                            locked_storage - required_storage;
  assert(storage_excess >= 0);

  unreleased_storage_cost_ = required_storage - committed_item_cost_;
  return storage_excess;
}

void
StorageItemDeltaManager::revert(StorageLock const &lock)
{
  assert(!round_committed_);
  [[maybe_unused]]
  auto initial_refcount = variant_.refcount();
  assert(initial_refcount > 0);

  switch (lock.lock_type()) {
  case StorageLockType::kNone:
    // impossible
    assert(false);
    break;
  case StorageLockType::kSetItemTypeClass:
    revert_set_typeclass_lock(lock.full_typeclass_lock().extra_lock_data);
    break;
  case StorageLockType::kExclusiveLock:
  case StorageLockType::kPermanentExclusiveLock:
    revert_exclusive_lock_to_none();
    break;
  }

  assert(variant_.refcount() == initial_refcount - 1);
}

Nondeterministic<std::optional<QueueEntry>>
StorageItemDeltaManager::get_concurrent_retrocausal_queue_entry(uint32_t index)
{
  // This is a read method, not a write method.  Therefore, this does not check
  // for locks held by this or other threads (including the
  // permanent_exclusive_lock_).

  if (!variant_.has_variant_arm<RetrocausalQueueArgs>()) {
    return std::nullopt;
  }

  assert(variant_.refcount() > 0);

  return variant_.get_variant_arm_unchecked<RetrocausalQueueArgs>()
      .get_concurrent_retrocausal_queue_entry(index);
}

} // namespace hogd
