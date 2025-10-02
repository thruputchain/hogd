#include "hogd/storage_item/storage_item_delta_manager.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/refcounted_delta_variant.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "hogd/storage_item/storage_lock_util.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"
#include "x23/format.h"

namespace hogd {

StorageItemDeltaManager::StorageItemDeltaManager(
    std::shared_ptr<const StorageItemWrapper> snapshot_state,
    InvalidStateFlag *invalid_state_flag, BlockExecutionMode execution_mode,
    StorageCostModel const *cost_model)
  : committed_item_(std::move(snapshot_state)),
    committed_item_cost_(committed_item_ ? committed_item_->storage_cost() : 0),
    invalid_state_flag_(invalid_state_flag), cost_model_(cost_model),
    execution_mode_(execution_mode)
{}

bool
StorageItemDeltaManager::try_reserve_nnint_set_add(int128_t set_value,
                                                   int128_t add_value)
{
  if (auto *args = variant_.try_set_variant<NNIntSetAddArgs>(set_value)) {
    if (set_value < 0 && add_value < 0) {
      // can only add to a set_value_ < 0
      variant_.release_reference();
      return false;
    }

    if (execution_mode_ == BlockExecutionMode::kBlockExecutionModeValidation) {
      return true;
    }

    // The assumption of this method is that there is no lock acquired yet on
    // this item. Thus, we acquire a new lock.

    NNIntBoundsLock bounds_lock{
        .add_amount = to_xdr(add_value > 0 ? add_value : 0),
        .subtract_amount = to_xdr(add_value < 0 ? add_value : 0),
    };

    if (args->try_acquire_bounds_lock(bounds_lock)) {
      return true;
    }
    variant_.release_reference();
    return false;
  }
  return false;
}

// Must be updated when adding a new delta type.
// StorageLockType::kNone -> other lock types
bool
StorageItemDeltaManager::try_reserve_delta(StorageDelta const &delta)
{
  assert(!variant_.has_permanent_exclusive_lock());

  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    return variant_.try_set_variant<BytestringArgs>(delta.bytestring()) !=
           nullptr;
  case StorageDeltaType::kDeltaTypeNonnegativeIntAdd:
  {
    int128_t base_value =
        [](StorageItemWrapper const *committed_item) -> int128_t {
      if (!committed_item) {
        return 0;
      }
      if (committed_item->type() != StorageItemType::kItemTypeNonnegativeInt) {
        return 0;
      }
      return committed_item->nnint();
    }(committed_item_.get());

    return try_reserve_nnint_set_add(base_value, from_xdr(delta.nnint_add()));
  }
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
    return try_reserve_nnint_set_add(from_xdr(delta.nnint().set_value),
                                     from_xdr(delta.nnint().add_value));
  case StorageDeltaType::kDeltaTypeDelete:
    return variant_.try_set_variant<DeleteArgs>() != nullptr;
  case StorageDeltaType::kDeltaTypeRetrocausalQueue:
  {
    bool requires_consume = delta.retrocausal_queue().consume != nullptr;
    if (auto *args = variant_.try_set_variant<RetrocausalQueueArgs>()) {
      if (requires_consume) {
        if (!args->try_reserve_consume()) {
          variant_.release_reference();
          return false;
        }
      }
      return true;
    }
    return false;
  }
  case StorageDeltaType::kDeltaTypeReplayCacheModify:
  {
    auto const *prev_cache =
        (committed_item_ &&
         committed_item_->type() == StorageItemType::kItemTypeReplayCache)
            ? &committed_item_->replay_cache()
            : nullptr;
    if (auto *args = variant_.try_set_variant<ReplayCacheArgs>(prev_cache)) {
      bool insert_failed = false;
      size_t insert_count = 0;
      for (; insert_count < delta.replay_cache_modify().nonces.size();
           insert_count++) {
        if (!args->try_reserve_insert(
                delta.replay_cache_modify().nonces[insert_count])) {
          insert_failed = true;
          break;
        }
      }
      if (!insert_failed) {
        return true;
      }
      for (size_t i = 0; i < insert_count; i++) {
        args->undo_insert(delta.replay_cache_modify().nonces[i]);
      }
      variant_.release_reference();
      return false;
    }
    return false;
  }
  case StorageDeltaType::kDeltaTypeAuthDataChange:
  {
    const auto *prev_auth_data =
        (committed_item_ &&
         committed_item_->type() == StorageItemType::kItemTypeAuthData)
            ? &committed_item_->auth_data()
            : nullptr;
    if (auto *args = variant_.try_set_variant<AuthDataArgs>(prev_auth_data)) {

      std::set<uint32_t> cleanup_keys = {};
      auto do_cleanup = [&]() {
        for (auto key_id : cleanup_keys) {
          args->undo_change_signer(key_id);
        }
        variant_.release_reference();
      };

      for (auto const &signer :
           delta.auth_data_change().auth_data_change_signers) {
        if (!args->try_reserve_change_signer(signer.public_key_id)) {
          do_cleanup();
          return false;
        }
        cleanup_keys.insert(signer.public_key_id);
      }
      return true;
    }
    return false;
  }
  case StorageDeltaType::kDeltaTypeDeployContract:
    return variant_.try_set_variant<DeployedContractArgs>(
               delta.deployed_contract_hash()) != nullptr;
  default:
    throw std::runtime_error("unknown delta type: " + x23::show(delta));
  }
}

// Must be updated when adding a new delta type.
void
StorageItemDeltaManager::commit_delta(StorageDelta const &delta,
                                      bool maintain_permanent_exclusive_lock)
{
  assert(variant_.refcount() > 0);
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    variant_.get_variant_arm_unchecked<BytestringArgs>().commit_set();
    break;
  case StorageDeltaType::kDeltaTypeNonnegativeIntAdd:
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
  {
    auto &args = variant_.get_variant_arm_unchecked<NNIntSetAddArgs>();
    bool valid_before = args.in_valid_state();
    auto add_value = [&delta] -> int128_t {
      if (delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntAdd) {
        return from_xdr(delta.nnint_add());
      }
      assert(delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
      return from_xdr(delta.nnint().add_value);
    }();
    args.commit_add(add_value);
    bool valid_after = args.in_valid_state();
    if (execution_mode_ == BlockExecutionMode::kBlockExecutionModeProposal) {
      assert(valid_before);
      assert(valid_after);
    }
    if (valid_before && !valid_after) {
      invalid_state_flag_->mark_as_invalid();
    }
    if (!valid_before && valid_after) {
      invalid_state_flag_->unmark_as_invalid();
    }
  } break;
  case StorageDeltaType::kDeltaTypeDelete:
    variant_.get_variant_arm_unchecked<DeleteArgs>().committed_ = true;
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
  case StorageDeltaType::kDeltaTypeReplayCacheModify:
  {
    auto &args = variant_.get_variant_arm_unchecked<ReplayCacheArgs>();
    args.commit_threshold_change(delta.replay_cache_modify().minimum_threshold);
    for (auto nonce : delta.replay_cache_modify().nonces) {
      args.commit_insert(nonce);
    }
  } break;
  case StorageDeltaType::kDeltaTypeAuthDataChange:
  {
    auto &args = variant_.get_variant_arm_unchecked<AuthDataArgs>();
    for (auto const &signer :
         delta.auth_data_change().auth_data_change_signers) {
      args.commit_change_signer(signer.public_key_id, signer.public_key.get());
    }
  } break;
  case StorageDeltaType::kDeltaTypeDeployContract:
    variant_.get_variant_arm_unchecked<DeployedContractArgs>().committed_ =
        true;
    break;
  default:
    throw std::runtime_error("unimplemented commit_delta");
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
    next_round_item_ = variant_.to_item(committed_item_, cost_model_);
    round_committed_ = true;
  }
}

// Should not need to be updated when adding a new delta type.
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
        variant_.get_marginal_cost_upper_bound(delta, cost_model_,
                                               committed_item_.get());

    return MeteredStorageLock{
        .lock_ = StorageLock(desired_lock_type),
        .locked_tokens_ = std::max<int128_t>(0, required_cost),
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
        variant_.get_marginal_cost_upper_bound(delta, cost_model_,
                                               committed_item_.get());

    return MeteredStorageLock{
        .lock_ = StorageLock(desired_lock_type),
        .locked_tokens_ = std::max<int128_t>(0, required_cost),
    };
  }
  case StorageLockType::kExclusiveLock:
  case StorageLockType::kPermanentExclusiveLock:
    // we already have the exclusive lock, so we can return true
    // This technically seems like acquiring the lock that we already have
    // but this lets the external logic be agnostic to whether we already have
    // the lock or not.
    auto required_cost =
        variant_.get_marginal_cost_upper_bound(delta, cost_model_,
                                               committed_item_.get());

    return MeteredStorageLock{
        .lock_ = StorageLock(desired_lock_type),
        .locked_tokens_ = std::max<int128_t>(0, required_cost),
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

  // handle type-specific lock data
  // This has to release the bounds lock for nnints, because case moves from
  // kExclusiveLock to a committed kSetItemTypeClass lock (and nnint is
  // retrocausal, so try_reserve_delta above acquires the lock, which is
  // normally released during commit() -- not in commit_delta() after this is
  // called).
  if (execution_mode_ == BlockExecutionMode::kBlockExecutionModeProposal) {
    if (delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd) {
      auto &args = variant_.get_variant_arm_unchecked<NNIntSetAddArgs>();
      int128_t add_value = from_xdr(delta.nnint().add_value);
      NNIntBoundsLock bounds_lock{
          .add_amount = to_xdr(add_value > 0 ? add_value : 0),
          .subtract_amount = to_xdr(add_value < 0 ? add_value : 0),
      };
      args.release_bounds_lock(bounds_lock);
    }
  }
}

// Must be updated when adding a new delta type.
void
StorageItemDeltaManager::revert_set_typeclass_lock(
    TypeSpecificLock const &type_specific_lock)
{
  switch (type_specific_lock.item_type()) {
  case StorageItemType::kItemTypeNonnegativeInt:
  {
    auto &args = variant_.get_variant_arm_unchecked<NNIntSetAddArgs>();
    if (execution_mode_ == BlockExecutionMode::kBlockExecutionModeProposal) {
      args.release_bounds_lock(type_specific_lock.nnint_bounds_lock());
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
  case StorageItemType::kItemTypeReplayCache:
  {
    auto &args = variant_.get_variant_arm_unchecked<ReplayCacheArgs>();
    for (auto nonce : type_specific_lock.nonces()) {
      args.undo_insert(nonce);
    }
    break;
  }
  case StorageItemType::kItemTypeBytestring:
  case StorageItemType::kItemTypeDeleted:
  case StorageItemType::kItemTypeDeployedContractHash:
    break;
  case StorageItemType::kItemTypeAuthData:
  {
    auto &args = variant_.get_variant_arm_unchecked<AuthDataArgs>();
    for (auto key_id : type_specific_lock.auth_data_lock().modified_key_ids) {
      args.undo_change_signer(key_id);
    }
    break;
  }
  default:
    throw std::runtime_error("unimplemented revert_set_typeclass_lock");
  }

  variant_.release_reference();
}

// Must be updated when adding a new delta type.
std::optional<StorageLock>
StorageItemDeltaManager::handle_try_reserve_from_set_typeclass_lock(
    StorageDelta const &delta, StorageLock const &prev_lock)
{

  auto const &prev_typeclass_lock = prev_lock.full_typeclass_lock();
  assert(variant_.refcount() > 0);
  assert(!variant_.has_permanent_exclusive_lock());

  auto req_lock = delta_to_lock(delta, execution_mode_, committed_item_.get());
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
    if (execution_mode_ == BlockExecutionMode::kBlockExecutionModeValidation) {
      // nothing to do in validation mode
      return req_lock;
    }
    // trying to upgrade one reservation lock to another
    auto const &prev_bounds_lock =
        prev_typeclass_lock.extra_lock_data.nnint_bounds_lock();
    int128_t new_add_amount = [&] {
      if (delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd) {
        return from_xdr(delta.nnint().add_value);
      }
      assert(delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
      return from_xdr(delta.nnint_add());
    }();

    NNIntBoundsLock req_bounds_lock{
        .add_amount = to_xdr(
            std::max(new_add_amount, from_xdr(prev_bounds_lock.add_amount))),
        .subtract_amount =
            to_xdr(std::min(new_add_amount,
                            from_xdr(prev_bounds_lock.subtract_amount))),
    };
    assert(variant_.has_variant_arm<NNIntSetAddArgs>());
    auto &args = variant_.get_variant_arm_unchecked<NNIntSetAddArgs>();

    args.release_bounds_lock(prev_bounds_lock);
    auto reserve_res = args.try_acquire_bounds_lock(req_bounds_lock);
    if (!reserve_res) {
      // reacquire the previous bounds lock
      [[maybe_unused]] auto r = args.try_acquire_bounds_lock(prev_bounds_lock);
      assert(r);
      return std::nullopt;
    }

    req_lock.full_typeclass_lock().extra_lock_data.nnint_bounds_lock() =
        req_bounds_lock;
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
  case StorageItemType::kItemTypeReplayCache:
  {
    assert(variant_.has_variant_arm<ReplayCacheArgs>());
    auto &args = variant_.get_variant_arm_unchecked<ReplayCacheArgs>();
    std::set<ReplayNonce> marginal_new_nonces = {
        delta.replay_cache_modify().nonces.begin(),
        delta.replay_cache_modify().nonces.end()};
    for (auto nonce : prev_typeclass_lock.extra_lock_data.nonces()) {
      marginal_new_nonces.erase(nonce);
    }

    bool insert_failed = false;
    auto it = marginal_new_nonces.begin();
    for (; it != marginal_new_nonces.end(); it++) {
      if (!args.try_reserve_insert(*it)) {
        insert_failed = true;
        break;
      }
    }
    if (insert_failed) {
      for (auto it2 = marginal_new_nonces.begin(); it2 != it; it2++) {
        args.undo_insert(*it2);
      }
      return std::nullopt;
    }
    return req_lock;
  }
  case StorageItemType::kItemTypeBytestring:
  case StorageItemType::kItemTypeDeleted:
  case StorageItemType::kItemTypeDeployedContractHash:
    return prev_lock;
  case StorageItemType::kItemTypeAuthData:
  {
    assert(variant_.has_variant_arm<AuthDataArgs>());
    auto &args = variant_.get_variant_arm_unchecked<AuthDataArgs>();
    std::set<uint32_t> cleanup_keys = {};

    auto do_cleanup = [&]() {
      for (auto key_id : cleanup_keys) {
        args.undo_change_signer(key_id);
      }
    };

    const auto &prev_modified_keys =
        prev_typeclass_lock.extra_lock_data.auth_data_lock().modified_key_ids;

    for (auto const &signer :
         delta.auth_data_change().auth_data_change_signers) {
      uint32_t key_id = signer.public_key_id;

      // Check if this key_id is already in the previous lock
      bool already_locked = std::ranges::find(prev_modified_keys, key_id) !=
                            prev_modified_keys.end();

      if (!already_locked) {
        if (!args.try_reserve_change_signer(key_id)) {
          do_cleanup();
          return std::nullopt;
        }
        cleanup_keys.insert(key_id);
        assert(std::ranges::contains(req_lock.full_typeclass_lock()
                                         .extra_lock_data.auth_data_lock()
                                         .modified_key_ids,
                                     key_id));
      }
    }

    for (auto key_id : prev_modified_keys) {
      if (!std::ranges::contains(req_lock.full_typeclass_lock()
                                     .extra_lock_data.auth_data_lock()
                                     .modified_key_ids,
                                 key_id)) {
        req_lock.full_typeclass_lock()
            .extra_lock_data.auth_data_lock()
            .modified_key_ids.push_back(key_id);
      }
    }

    return req_lock;
  }
  default:
    throw std::runtime_error(
        "unimplemented handle_try_reserve_from_set_typeclass_lock");
  }
}

std::optional<MeteredStorageLock>
StorageItemDeltaManager::try_reserve(StorageDelta const &delta,
                                     StorageLock const &previous_lock)
{
  trace("Trying to reserve delta {} on lock {}", x23::show(delta), x23::show(previous_lock));
  assert(!round_committed_);
  switch (previous_lock.lock_type()) {
  case StorageLockType::kNone:
  {
    if (variant_.has_permanent_exclusive_lock()) {
      return std::nullopt;
    }
    if (try_reserve_delta(delta)) {

      int128_t required_storage =
          variant_.get_marginal_cost_upper_bound(delta, cost_model_,
                                                 committed_item_.get());
      return MeteredStorageLock{.lock_ = delta_to_lock(delta, execution_mode_,
                                                       committed_item_.get()),
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
        variant_.get_marginal_cost_upper_bound(delta, cost_model_,
                                               committed_item_.get());

    return MeteredStorageLock{.lock_ = *out,
                              .locked_tokens_ =
                                  std::max<int128_t>(required_storage, 0)};
  }
  // always succeeds if we have an exclusive lock
  case StorageLockType::kExclusiveLock:
  case StorageLockType::kPermanentExclusiveLock:
  {
    auto required_cost =
        variant_.get_marginal_cost_upper_bound(delta, cost_model_,
                                               committed_item_.get());

    return MeteredStorageLock{.lock_ = previous_lock,
                              .locked_tokens_ =
                                  std::max<int128_t>(required_cost, 0)};
  }
  default:
    throw std::runtime_error("impossible");
  }
}

// Must be updated when adding a new delta type.
void
StorageItemDeltaManager::revert_set_typeclass_lock_to_set_typeclass(
    StorageLock const &lock, StorageDelta const &delta)
{
  assert(lock.lock_type() == StorageLockType::kSetItemTypeClass);
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    // nothing to do
    break;
  case StorageDeltaType::kDeltaTypeNonnegativeIntAdd:
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
  {
    if (execution_mode_ == BlockExecutionMode::kBlockExecutionModeValidation) {
      // nothing to do in validation mode
      return;
    }
    assert(lock.full_typeclass_lock().extra_lock_data.item_type() ==
           StorageItemType::kItemTypeNonnegativeInt);

    auto &args = variant_.get_variant_arm_unchecked<NNIntSetAddArgs>();

    args.release_bounds_lock(
        lock.full_typeclass_lock().extra_lock_data.nnint_bounds_lock());

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
  case StorageDeltaType::kDeltaTypeReplayCacheModify:
  {
    auto &args = variant_.get_variant_arm_unchecked<ReplayCacheArgs>();
    for (auto nonce : lock.full_typeclass_lock().extra_lock_data.nonces()) {
      args.undo_insert(nonce);
    }
    for (auto nonce : delta.replay_cache_modify().nonces) {
      [[maybe_unused]] auto r = args.try_reserve_insert(nonce);
      assert(r);
    }
    break;
  }
  case StorageDeltaType::kDeltaTypeAuthDataChange:
  {
    auto &args = variant_.get_variant_arm_unchecked<AuthDataArgs>();

    // Release the lock for this key ID, then re-acquire it
    const auto &prev_modified_keys = lock.full_typeclass_lock()
                                         .extra_lock_data.auth_data_lock()
                                         .modified_key_ids;
    for (auto const &key_id : prev_modified_keys) {
      args.undo_change_signer(key_id);
    }
    for (auto const &signer :
         delta.auth_data_change().auth_data_change_signers) {
      [[maybe_unused]] auto r =
          args.try_reserve_change_signer(signer.public_key_id);
      assert(r);
    }
    break;
  }
  case StorageDeltaType::kDeltaTypeDeployContract:
    // nothing to do
    break;
  default:
    throw std::runtime_error(
        "unimplemented revert_set_typeclass_lock_to_set_typeclass");
  }
}

int128_t
StorageItemDeltaManager::commit(StorageDelta const &delta,
                                MeteredStorageLock const &lock)
{
  int128_t previous_committed_cost =
      variant_.get_current_committed_cost(cost_model_, committed_item_.get());

  assert(!round_committed_);
  assert(lock.lock_.lock_type() != StorageLockType::kNone);

  switch (lock.lock_.lock_type()) {
  case StorageLockType::kSetItemTypeClass:
    revert_set_typeclass_lock_to_set_typeclass(lock.lock_, delta);
    commit_delta(delta, false);
    break;
  case StorageLockType::kExclusiveLock:
    revert_exclusive_lock_to_set_typeclass(delta);
    commit_delta(delta, false);
    break;
  case StorageLockType::kPermanentExclusiveLock:
    revert_exclusive_lock_to_set_typeclass(delta);
    commit_delta(delta, true);
    break;
  case StorageLockType::kNone:
    assert(false);
    throw std::runtime_error("impossible");
  }

  assert(!variant_.has_variant_arm<ExclusiveLockedState>());

  int128_t required_storage =
      variant_.get_current_committed_cost(cost_model_, committed_item_.get());
  assert(required_storage >= 0);

  int128_t locked_storage = lock.locked_tokens_;
  assert(previous_committed_cost + locked_storage >= required_storage);

  int128_t storage_excess =
      previous_committed_cost + locked_storage - required_storage;
  assert(storage_excess >= 0);

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
