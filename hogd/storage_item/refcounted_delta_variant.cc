#include "hogd/storage_item/refcounted_delta_variant.h"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "account.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/replay_cache.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

int128_t
get_marginal_item_creation_bound(StorageCostModel const *current_model,
                                 StorageItemWrapper const *base_item)
{
  if (!base_item) {
    return current_model->kBaseItemCost;
  }
  return std::max<int128_t>(0, current_model->kBaseItemCost -
                                   base_item->cost_model()->kBaseItemCost);
}

std::shared_ptr<const StorageItemWrapper>
BytestringArgs::to_item(StorageCostModel const *cost_model) const
{
  assert(committed_);
  return std::make_shared<const StorageItemWrapper>(bytestring_set_,
                                                    cost_model);
}

int128_t
BytestringArgs::get_current_committed_cost(
    StorageCostModel const *current_model,
    StorageItemWrapper const *base_item) const
{
  assert(current_model);
  if (committed_) {
    return current_model->get_bytestring_cost(bytestring_set_.size());
  }
  if (base_item) {
    return base_item->storage_cost();
  }
  return 0;
}

int128_t
BytestringArgs::get_marginal_cost_upper_bound(
    StorageDelta const &delta, StorageCostModel const *current_model,
    StorageItemWrapper const *base_item) const
{
  return get_marginal_cost_upper_bound_static(committed_, delta, current_model,
                                              base_item);
}
int128_t
BytestringArgs::get_marginal_cost_upper_bound_static(
    bool is_committed, StorageDelta const &delta,
    StorageCostModel const *current_model, StorageItemWrapper const *base_item)
{
  if (is_committed) {
    return 0;
  }
  int128_t locked_amount = base_item ? base_item->storage_cost() : 0;
  int128_t new_cost =
      current_model->get_bytestring_cost(delta.bytestring().size());
  return std::max<int128_t>(0, new_cost - locked_amount);
}

std::shared_ptr<const StorageItemWrapper>
NNIntSetAddArgs::to_item(StorageCostModel const *cost_model) const
{
  assert(lower_bound_.is_in_range<int128_t>() &&
         upper_bound_.is_in_range<int128_t>());
  auto new_value = final_value_.get_capped<int128_t>();
  // assertions fails if there were locks that were acquired but not releases
  assert(new_value == upper_bound_.get_capped<int128_t>());
  assert(new_value == lower_bound_.get_capped<int128_t>());

  return std::make_shared<const StorageItemWrapper>(new_value, cost_model);
}

void
NNIntSetAddArgs::release_bounds_lock(NNIntBoundsLock const &bounds_lock)
{
  assert(from_xdr(bounds_lock.add_amount) >= 0);
  assert(from_xdr(bounds_lock.subtract_amount) <= 0);

  lower_bound_ -= from_xdr(bounds_lock.subtract_amount);
  upper_bound_ -= from_xdr(bounds_lock.add_amount);
}

bool
NNIntSetAddArgs::try_acquire_bounds_lock(NNIntBoundsLock const &bounds_lock)
{
  assert(from_xdr(bounds_lock.add_amount) >= 0);
  assert(from_xdr(bounds_lock.subtract_amount) <= 0);

  if (set_value_ < 0 && from_xdr(bounds_lock.subtract_amount) < 0) {
    // can only add to a set_value_ < 0
    return false;
  }

  lower_bound_ += from_xdr(bounds_lock.subtract_amount);
  upper_bound_ += from_xdr(bounds_lock.add_amount);

  auto undo_lock = [this, bounds_lock]() {
    lower_bound_ -= from_xdr(bounds_lock.subtract_amount);
    upper_bound_ -= from_xdr(bounds_lock.add_amount);
  };

  // assert both bounds are in [min(0, set_value_), INT128_MAX]
  if (!lower_bound_.is_in_range<int128_t>() ||
      !upper_bound_.is_in_range<int128_t>()) {
    undo_lock();
    return false;
  }

  auto lb = lower_bound_.get_capped<int128_t>();
  if (set_value_ < 0) {
    // can't acquire negative locks on a set_value_ < 0
    assert(lb >= set_value_);
    return true;
  }
  if (lb >= 0) {
    return true;
  }

  // undo the above operations and return false
  undo_lock();
  return false;
}

void
NNIntSetAddArgs::commit_add(int128_t add_value)
{
  committed_ = true;
  lower_bound_ += add_value;
  upper_bound_ += add_value;
  final_value_ += add_value;
}

bool
NNIntSetAddArgs::in_valid_state() const
{
  if (!final_value_.is_in_range<int128_t>()) {
    return false;
  }

  auto val = final_value_.get_capped<int128_t>();
  return (val >= std::min<int128_t>(set_value_, 0));
}

namespace {
x23::Vec<QueueEntry>
make_block(std::multiset<QueueEntry> const &entries)
{
  x23::Vec<QueueEntry> new_block;
  for (auto entry : entries) {
    new_block.emplace_back(std::move(entry));
  }
  return new_block;
}
} // namespace

std::shared_ptr<const StorageItemWrapper>
RetrocausalQueueArgs::to_item(const StorageItemWrapper *previous_item,
                              StorageCostModel const *cost_model) const
{
  assert(!consumer_lock_);

  RetrocausalQueuePastBlockConsumeDelta consume_delta = kEmptyConsume;
  if (past_consume_delta_) {
    if (previous_item) { // NOLINT(bugprone-branch-clone)
      assert(previous_item->type() ==
             StorageItemType::kItemTypeRetrocausalQueue);
    }
    else {
      assert(past_consume_delta_ == kEmptyConsume);
    }
    consume_delta = *past_consume_delta_;
  }

  auto make_output = [&](RetrocausalQueue const &parent) {
    uint64_t new_block_number = parent.get_current_block_number() + 1;
    x23::Vec<QueueEntry> new_block = make_block(persistable_entries_);
    if (!new_block.empty()) {
      return std::make_shared<const StorageItemWrapper>(
          RetrocausalQueue(parent, consume_delta,
                           RetrocausalQueueBlock{.entries = new_block,
                                                 .block_number =
                                                     new_block_number}),
          cost_model);
    }
    return std::make_shared<const StorageItemWrapper>(
        RetrocausalQueue(parent, consume_delta), cost_model);
  };

  if (previous_item &&
      previous_item->type() == StorageItemType::kItemTypeRetrocausalQueue) {
    return make_output(previous_item->queue());
  }
  // otherwise, no parent, so make empty one just for the ctor
  RetrocausalQueue fake_parent(0);
  assert(!past_consume_delta_ || *past_consume_delta_ == kEmptyConsume);

  return make_output(fake_parent);
}

int128_t
RetrocausalQueueArgs::get_current_committed_cost(
    StorageCostModel const *current_model,
    StorageItemWrapper const *base_item) const
{
  assert(current_model);
  // nothing has committed yet
  if (!insert_committed_ && !past_consume_delta_) {
    if (base_item) {
      return base_item->storage_cost();
    }
    return 0;
  }

  auto stats = get_current_committed_stats(base_item);
  return current_model->kBaseItemCost +
         current_model->get_retrocausal_queue_cost(stats.active_size,
                                                   stats.active_block_count);
}

int128_t
RetrocausalQueueArgs::get_marginal_cost_upper_bound(
    StorageDelta const &delta, StorageCostModel const *current_model,
    StorageItemWrapper const *base_item) const
{
  assert(delta.type() == StorageDeltaType::kDeltaTypeRetrocausalQueue);

  // TODO(geoff): Because the queue is single-consumer,
  // we can grant a refund to the consumer immediately;
  // This doesn't mean returning a negative cost, but rather
  // that a delta that consumes 100 entries and
  // creates 100 entries can have a net cost of 0.
  // this requires checking over the past stats, which we have not implemented
  // yet here.
  /*
   auto past_stats = [&] {
     if (base_item) {
       assert(base_item->type() == StorageItemType::kItemTypeRetrocausalQueue);
       auto const &queue = base_item->queue();

       if (past_consume_delta_) {
         assert(!delta.retrocausal_queue().consume);
         return queue.get_stats_after_consume(*past_consume_delta_);
       }
       if (delta.retrocausal_queue().consume) {
         assert(consumer_lock_);
         return
   queue.get_stats_after_consume(delta.retrocausal_queue().consume->past_block_consume);
       }
       return queue.get_active_stats();
     }
     return RetrocausalQueueStats{.active_block_count = 0, .active_size = 0};
   }(); */

  int64_t new_inserted_entries =
      delta.retrocausal_queue().insert.added_entries.size();
  if (delta.retrocausal_queue().consume) {
    new_inserted_entries -=
        delta.retrocausal_queue().consume->current_block_consumed.size();
  }

  // We can only create a new block if we insert entries
  bool new_block_maybe_created =
      !delta.retrocausal_queue().insert.added_entries.empty();
  if (past_consume_delta_.has_value() && !persistable_entries_.empty()) {
    // if we've already done the consume for this block and still have a
    // nonempty block to add, no need to reserve creating a new block
    new_block_maybe_created = false;
  }

  RetrocausalQueueStats new_stats = {
      .active_block_count = new_block_maybe_created ? 1U : 0U,
      .active_size = std::max<int64_t>(0, new_inserted_entries)};

  bool exists_committed_delta =
      insert_committed_ || past_consume_delta_.has_value();

  int128_t base_cost = [&] -> int128_t {
    if (exists_committed_delta) {
      return 0;
    }
    if (base_item) {
      return std::max<int128_t>(0, base_item->storage_cost(current_model) -
                                       base_item->storage_cost());
    }
    return current_model->kBaseItemCost;
  }();

  return base_cost + current_model->get_retrocausal_queue_cost(
                         new_stats.active_size, new_stats.active_block_count);
}

RetrocausalQueueStats
RetrocausalQueueArgs::get_current_committed_stats(
    StorageItemWrapper const *base_item) const
{
  auto past_stats = [&] {
    if (base_item) {
      assert(base_item->type() == StorageItemType::kItemTypeRetrocausalQueue);
      auto const &queue = base_item->queue();

      if (past_consume_delta_) {
        return queue.get_stats_after_consume(*past_consume_delta_);
      }
      return queue.get_active_stats();
    }
    return RetrocausalQueueStats{.active_block_count = 0, .active_size = 0};
  }();

  int64_t current_block_size =
      persistable_entries_.size() - consumed_but_not_inserted_entries_.size();

  RetrocausalQueueStats new_stats = {.active_block_count =
                                         (persistable_entries_.empty()) ? 0U
                                                                        : 1U,
                                     .active_size = current_block_size};

  return RetrocausalQueueStats{.active_block_count =
                                   past_stats.active_block_count +
                                   new_stats.active_block_count,
                               .active_size = past_stats.active_size +
                                              new_stats.active_size};
}

void
RetrocausalQueueArgs::commit_consume(
    RetrocausalQueueConsumeDelta const &consume_delta,
    InvalidStateFlag &invalid_state_flag)
{
  assert(consumer_lock_);
  assert(!past_consume_delta_.has_value());
  consumer_lock_ = false;
  past_consume_delta_ = consume_delta.past_block_consume;

  for (auto const &consumed_entry : consume_delta.current_block_consumed) {
    auto res = persistable_entries_.erase(consumed_entry);
    if (res == 0) {
      if (consumed_but_not_inserted_entries_.empty()) {
        invalid_state_flag.mark_as_invalid();
      }
      consumed_but_not_inserted_entries_.insert(consumed_entry);
    }
  }
}

void
RetrocausalQueueArgs::commit_insert(
    RetrocausalQueueInsertDelta const &insert_delta,
    InvalidStateFlag &invalid_state_flag)
{
  for (auto const &entry : insert_delta.added_entries) {
    auto it = consumed_but_not_inserted_entries_.find(entry);
    if (it == consumed_but_not_inserted_entries_.end()) {
      persistable_entries_.insert(entry);
    }
    else {
      // consumed_but_not_inserted_entries is not empty right now, as we just
      // found an item in it.
      consumed_but_not_inserted_entries_.erase(it);
      if (consumed_but_not_inserted_entries_.empty()) {
        invalid_state_flag.unmark_as_invalid();
      }
    }
    new_entries_.push_back(entry);
  }
  insert_committed_ = true;
}

std::optional<QueueEntry>
RetrocausalQueueArgs::get_concurrent_retrocausal_queue_entry(
    uint32_t index) const
{
  if (index < new_entries_.size()) {
    return new_entries_[index];
  }
  return std::nullopt;
}

bool
ReplayCacheArgs::try_reserve_insert(ReplayNonce nonce)
{
  if (base_cache_ && !base_cache_->can_accept(nonce)) {
    return false;
  }
  auto [_, inserted] = new_nonces_.insert(nonce);
  return inserted;
}

void
ReplayCacheArgs::undo_insert(ReplayNonce nonce)
{
  [[maybe_unused]] auto res = new_nonces_.erase(nonce);
  assert(res == 1);
}

std::shared_ptr<const StorageItemWrapper>
ReplayCacheArgs::to_item(StorageCostModel const *cost_model) const
{
  std::set<ReplayNonce> nonces =
      base_cache_ ? base_cache_->nonces() : std::set<ReplayNonce>();
  ReplayNonce min_nonce =
      std::max<ReplayNonce>(metadata_.minimum_threshold,
                            base_cache_
                                ? base_cache_->metadata().minimum_threshold
                                : 0);
  auto it = nonces.upper_bound(min_nonce);
  nonces.erase(nonces.begin(), it);

  nonces.insert(committed_nonces_.begin(), committed_nonces_.end());

  ReplayCacheMetadata new_metadata = {.minimum_threshold = min_nonce};

  return std::make_shared<const StorageItemWrapper>(ReplayCache{new_metadata,
                                                                nonces},
                                                    cost_model);
}

int128_t
ReplayCacheArgs::get_current_committed_cost(
    StorageCostModel const *current_model,
    StorageItemWrapper const *base_item) const
{
  if (!has_committed_delta_) {
    if (base_item) {
      return base_item->storage_cost();
    }
    return 0;
  }

  size_t committed_size = committed_nonces_.size();
  if (base_item) {
    committed_size += base_item->replay_cache().count_remaining_after_clear(
        metadata_.minimum_threshold);
  }
  return current_model->kBaseItemCost +
         current_model->get_replay_cache_cost(committed_size);
}

int128_t
ReplayCacheArgs::get_marginal_cost_upper_bound_static(
    bool is_committed, StorageDelta const &delta,
    StorageCostModel const *current_model, StorageItemWrapper const *base_item)
{
  int128_t base_cost = current_model->kBaseItemCost;
  if (is_committed || base_item) {
    base_cost = 0;
  }

  return base_cost + current_model->get_replay_cache_cost(
                         delta.replay_cache_modify().nonces.size());
}

int128_t
ReplayCacheArgs::get_marginal_cost_upper_bound(
    StorageDelta const &delta, StorageCostModel const *current_model,
    StorageItemWrapper const *base_item) const
{
  assert(delta.type() == StorageDeltaType::kDeltaTypeReplayCacheModify);
  return get_marginal_cost_upper_bound_static(!committed_nonces_.empty() ||
                                                  has_committed_delta_,
                                              delta, current_model, base_item);
}

/*
int128_t
ReplayCacheArgs::get_cost(ReplayCacheModifyArgs const &delta) const
{
  auto new_min = std::max<ReplayNonce>(
      {metadata_.minimum_threshold, delta.minimum_threshold,
       base_cache_ ? base_cache_->metadata().minimum_threshold : 0});

  size_t prev_count = 0;
  if (base_cache_) {
    auto const &nonces = base_cache_->nonces();
    auto it = nonces.upper_bound(new_min);
    prev_count = std::distance(it, nonces.end());
  }

  auto it = new_nonces_.upper_bound(new_min);
  size_t new_count = std::distance(it, new_nonces_.end());

  return kBaseItemCost + replay_cache_cost(new_count + prev_count);
} */

void
ReplayCacheArgs::commit_threshold_change(ReplayNonce new_threshold)
{
  metadata_.minimum_threshold =
      std::max<ReplayNonce>({new_threshold, metadata_.minimum_threshold});
  auto bound = committed_nonces_.upper_bound(new_threshold);
  committed_nonces_.erase(committed_nonces_.begin(), bound);
}

bool
AuthDataArgs::try_reserve_change_signer(uint32_t key_id)
{
  if (key_id >= MAX_SIGNERS) {
    return false;
  }
  auto [_, inserted] = modified_key_ids_.insert(key_id);
  return inserted;
}

void
AuthDataArgs::undo_change_signer(uint32_t key_id)
{
  [[maybe_unused]] auto res = modified_key_ids_.erase(key_id);
  assert(res == 1);
}

void
AuthDataArgs::commit_change_signer(uint32_t key_id,
                                   const PublicKeyConfig *new_key)
{
  assert(modified_key_ids_.contains(key_id));
  if (new_key) {
    // Create a PublicKeyConfig with the new key and default weight
    committed_changes_[key_id] =
        x23::Ptr<PublicKeyConfig>(new PublicKeyConfig(*new_key));
  }
  else {
    committed_changes_[key_id] = nullptr;
  }
  has_committed_delta_ = true;
}

std::shared_ptr<const StorageItemWrapper>
AuthDataArgs::to_item(StorageCostModel const *cost_model) const
{
  AccountAuthData new_auth_data;
  if (base_auth_data_) {
    new_auth_data = *base_auth_data_;
  }
  else {
    // Initialize default auth data
    for (size_t i = 0; i < MAX_SIGNERS; ++i) {
      new_auth_data.public_keys[i] = nullptr;
    }
  }

  // Apply committed changes
  for (const auto &[key_id, new_key] : committed_changes_) {
    if (key_id < MAX_SIGNERS) {
      new_auth_data.public_keys[key_id] = new_key;
    }
  }

  return std::make_shared<const StorageItemWrapper>(new_auth_data, cost_model);
}

int128_t
AuthDataArgs::get_current_committed_cost( // NOLINT
    StorageCostModel const *current_model,
    StorageItemWrapper const *base_item) const
{
  assert(current_model);
  if (has_committed_delta_) {
    // TODO(geoff): Implement proper cost model for auth data based on number of
    // signers
    return current_model->kBaseItemCost;
  }

  if (base_item) {
    return base_item->storage_cost();
  }
  return 0;
}

int128_t
AuthDataArgs::get_marginal_cost_upper_bound_static(
    bool is_committed, StorageDelta const & /*delta*/,
    StorageCostModel const *current_model, StorageItemWrapper const *base_item)
{
  int128_t base_cost = current_model->kBaseItemCost;
  if (is_committed || base_item) {
    base_cost = 0;
  }

  // TODO(geoff): Consider marginal cost for adding/removing signers
  return base_cost;
}

int128_t
AuthDataArgs::get_marginal_cost_upper_bound(
    StorageDelta const &delta, StorageCostModel const *current_model,
    StorageItemWrapper const *base_item) const
{
  assert(delta.type() == StorageDeltaType::kDeltaTypeAuthDataChange);
  return get_marginal_cost_upper_bound_static(has_committed_delta_, delta,
                                              current_model, base_item);
}

int128_t
DeployedContractArgs::get_current_committed_cost(
    StorageCostModel const *cost_model,
    StorageItemWrapper const *base_item) const
{
  assert(cost_model);
  if (committed_) {
    if (base_item) {
      warn("Redeploying a contract; that's not supposed to happen");
      assert(base_item->deployed_contract_hash() == deployed_contract_hash_);
    }
    return cost_model->get_contract_size_cost(
        deployed_contract_hash_.contract_size);
  }
  if (base_item) {
    assert(base_item->type() == StorageItemType::kItemTypeDeployedContractHash);
    return base_item->storage_cost();
  }
  return 0;
}

int128_t
DeployedContractArgs::get_marginal_cost_upper_bound(
    StorageDelta const &delta, StorageCostModel const *cost_model,
    StorageItemWrapper const * /*unused*/)
{
  assert(delta.type() == StorageDeltaType::kDeltaTypeDeployContract);
  return cost_model->get_contract_size_cost(
      delta.deployed_contract_hash().contract_size);
}

std::shared_ptr<const StorageItemWrapper>
DeployedContractArgs::to_item(StorageCostModel const *cost_model) const
{
  return std::make_shared<const StorageItemWrapper>(deployed_contract_hash_,
                                                    cost_model);
}

int128_t
ExclusiveLockedState::get_marginal_cost_upper_bound(
    StorageDelta const &delta, StorageCostModel const *cost_model,
    StorageItemWrapper const *base_item)
{
  // TODO(geoff): Most of these methods could be factored out into a static
  // and nonstatic version
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    return BytestringArgs::get_marginal_cost_upper_bound_static(false, delta,
                                                                cost_model,
                                                                base_item);
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
    return NNIntSetAddArgs::get_marginal_cost_upper_bound_static(false,
                                                                 cost_model,
                                                                 base_item);
  case StorageDeltaType::kDeltaTypeDelete:
    return DeleteArgs::get_marginal_cost_upper_bound(delta, cost_model,
                                                     base_item);
  case StorageDeltaType::kDeltaTypeRetrocausalQueue:
  {
    RetrocausalQueueArgs args;
    return args.get_marginal_cost_upper_bound(delta, cost_model, base_item);
  }
  case StorageDeltaType::kDeltaTypeReplayCacheModify:
    return ReplayCacheArgs::get_marginal_cost_upper_bound_static(false, delta,
                                                                 cost_model,
                                                                 base_item);
  case StorageDeltaType::kDeltaTypeAuthDataChange:
    return AuthDataArgs::get_marginal_cost_upper_bound_static(false, delta,
                                                              cost_model,
                                                              base_item);
  default:
    throw std::runtime_error("Impossible state : delta is " + x23::show(delta));
  }
}

bool
RefcountedDeltaVariant::matches_typeclass(StorageItemTypeClass typeclass) const
{
  assert(refcount_ > 0);

  return std::visit(
      [&typeclass](auto const &args) {
        using args_t = std::decay_t<decltype(args)>;
        if constexpr (std::same_as<args_t, DeleteArgs>) {
          return typeclass.item_type() == StorageItemType::kItemTypeDeleted &&
                 args.typeclass_match();
        }
        else if constexpr (std::same_as<args_t, BytestringArgs>) {
          return typeclass.item_type() ==
                     StorageItemType::kItemTypeBytestring &&
                 args.typeclass_match(typeclass.bytestring());
        }
        else if constexpr (std::same_as<args_t, NNIntSetAddArgs>) {
          return typeclass.item_type() ==
                     StorageItemType::kItemTypeNonnegativeInt &&
                 args.typeclass_match(from_xdr(typeclass.nnint_set()));
        }
        else if constexpr (std::same_as<args_t, RetrocausalQueueArgs>) {
          return typeclass.item_type() ==
                     StorageItemType::kItemTypeRetrocausalQueue &&
                 args.typeclass_match();
        }
        else if constexpr (std::same_as<args_t, ReplayCacheArgs>) {
          // hack because the signature of the ctor has to match the
          // typeclass_match signature in this implemebntation
          return typeclass.item_type() ==
                     StorageItemType::kItemTypeReplayCache &&
                 args.typeclass_match(nullptr);
        }
        else if constexpr (std::same_as<args_t, AuthDataArgs>) {
          // hack because the signature of the ctor has to match the
          // typeclass_match signature in this implementation
          return typeclass.item_type() == StorageItemType::kItemTypeAuthData &&
                 args.typeclass_match(nullptr);
        }
        else if constexpr (std::same_as<args_t, DeployedContractArgs>) {
          return typeclass.item_type() ==
                     StorageItemType::kItemTypeDeployedContractHash &&
                 args.typeclass_match(typeclass.deployed_contract_hash());
        }
        else {
          static_assert(
              std::same_as<args_t, ExclusiveLockedState> ||
                  std::same_as<args_t, UnmodifiedState>,
              "should never match exclusive locked state or unmodified state");
          return false;
        }
      },
      variant_);
}

std::shared_ptr<const StorageItemWrapper>
RefcountedDeltaVariant::to_item(
    std::shared_ptr<const StorageItemWrapper> previous_item,
    StorageCostModel const *cost_model) const
{
  if (refcount_ > 0) {
    return std::visit(
        [&](auto &arm) {
          if constexpr (requires(decltype(arm) &a) { a.to_item(); }) {
            return arm.to_item();
          }
          else if constexpr (requires(decltype(arm) &a,
                                      const StorageItemWrapper *b,
                                      const StorageCostModel *c) {
                               a.to_item(b, c);
                             }) {
            return arm.to_item(previous_item.get(), cost_model);
          }
          else if constexpr (requires(
                                 decltype(arm) &a,
                                 std::shared_ptr<const StorageItemWrapper> b) {
                               a.to_item(b);
                             }) {
            return arm.to_item(previous_item);
          }
          else {
            return arm.to_item(cost_model);
          }
        },
        variant_);
  }
  return previous_item;
}

int128_t
RefcountedDeltaVariant::get_current_committed_cost(
    StorageCostModel const *current_model,
    StorageItemWrapper const *base_item) const
{
  return std::visit(
      [&](auto const &args) {
        return args.get_current_committed_cost(current_model, base_item);
      },
      variant_);
}

int128_t
RefcountedDeltaVariant::get_marginal_cost_upper_bound(
    StorageDelta const &delta, StorageCostModel const *current_model,
    StorageItemWrapper const *base_item) const
{
  assert(refcount_ > 0);
  int128_t out = std::visit(
      [&](auto const &args) {
        return args.get_marginal_cost_upper_bound(delta, current_model,
                                                  base_item);
      },
      variant_);
  assert(out >= 0);
  return out;
}

} // namespace hogd
