#include "hogd/storage_item/refcounted_delta_variant.h"

#include <cassert>
#include <concepts>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "hogd/storage_item/storage_costs.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "x23/vec.h"

namespace hogd {

std::shared_ptr<const StorageItemWrapper>
BytestringArgs::to_item() const
{
  return std::make_shared<const StorageItemWrapper>(
      StorageItemWrapper::BytestringMarker{}, bytestring_set_);
}

std::shared_ptr<const StorageItemWrapper>
NNIntSetAddArgs::to_item() const
{
  int128_t new_value = set_value_ + total_subtracted_;
  int128_t total_add = total_added_.get_capped();

  int128_t out = 0;

  if (integer_add_overflow<int128_t>(new_value, total_add)) {
    out = std::numeric_limits<int128_t>::max();
  }
  else {
    out = new_value + total_add;
  }

  return std::make_shared<const StorageItemWrapper>(
      StorageItemWrapper::NonnegativeIntMarker{}, out);
}

bool
NNIntSetAddArgs::try_reserve_add(int128_t add_value)
{
  int128_t desired_subtracted = 0;
  assert(add_value < 0);

  if (integer_add_overflow(total_subtracted_, add_value, &desired_subtracted)) {
    // more than INT128_MIN is subtracted from the counter.
    // This is impossible, if we want to maintain that set+add >= 0.
    return false;
  }

  // desired_subtracted < 0, set_value > 0 => no overflow on addition
  if (desired_subtracted + set_value_ < 0) {
    // this new subtraction would drive the value below 0
    return false;
  }

  total_subtracted_ = desired_subtracted;
  return true;
}
void
NNIntSetAddArgs::commit_add(int128_t add_value)
{
  assert(add_value > 0);
  total_added_ += add_value;
}

void
NNIntSetAddArgs::revert_add(int128_t add_value)
{
  assert(add_value < 0);
  total_subtracted_ -= add_value;
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
RetrocausalQueueArgs::to_item(const StorageItemWrapper *previous_item) const
{
  assert(!consumer_lock_);

  RetrocausalQueuePastBlockConsumeDelta consume_delta{
      .max_fully_consumed_block_number = 0, .partial_consumed_number = 0};
  if (past_consume_delta_) {
    if (previous_item) {
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
    if (insert_committed_) {
      return std::make_shared<const StorageItemWrapper>(
          StorageItemWrapper::RetrocausalQueueMarker{},
          RetrocausalQueue(parent, consume_delta,
                           RetrocausalQueueBlock{.entries = new_block,
                                                 .block_number =
                                                     new_block_number}));
    }
    return std::make_shared<const StorageItemWrapper>(
        StorageItemWrapper::RetrocausalQueueMarker{},
        RetrocausalQueue(parent, consume_delta));
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
RetrocausalQueueArgs::get_cost(RetrocausalQueueDelta const &delta,
                               const StorageItemWrapper *base_item) const
{
  auto consume_delta =
      [&] -> std::optional<RetrocausalQueuePastBlockConsumeDelta> {
    if (past_consume_delta_) {
      assert(!delta.consume);
      return past_consume_delta_;
    }
    if (delta.consume) {
      assert(consumer_lock_);
      return delta.consume->past_block_consume;
    }
    return std::nullopt;
  }();

  auto past_stats = [&] {
    if (base_item) {
      assert(base_item->type() == StorageItemType::kItemTypeRetrocausalQueue);
      auto const &queue = base_item->queue();

      if (consume_delta) {
        return queue.get_stats_after_consume(*consume_delta);
      }
      return queue.get_active_stats();
    }
    return RetrocausalQueueStats{.active_block_count = 0, .active_size = 0};
  }();

  int64_t new_entry_count =
      delta.insert.added_entries.size() + persistable_entries_.size();
  int64_t consumed_entry_count =
      consumed_but_not_inserted_entries_.size() +
      (delta.consume ? delta.consume->current_block_consumed.size() : 0);

  // The accounting logic is vastly simplified by the following trick:
  // If there are new entries in a block but they're all retrocausally consumed,
  // then, logically speaking, there isn't a new block, but we still count it
  // for storage costs until a later consume.

  RetrocausalQueueStats new_stats = {
      .active_block_count =
          (new_entries_.empty() && delta.insert.added_entries.empty()) ? 0U
                                                                       : 1U,
      .active_size = new_entry_count - consumed_entry_count};

  return kBaseItemCost + retrocausal_queue_cost(past_stats) +
         retrocausal_queue_cost(new_stats);
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

std::optional<AssetBalanceLock>
AssetBalanceArgs::try_reserve_transfer_lock(int128_t new_transfer_amount,
                                            AssetBalanceLock const &held_lock)
{
  if (new_transfer_amount > 0) {
    if (from_xdr(held_lock.asset_add_amount) >= new_transfer_amount) {
      return held_lock;
    }
    // otherwise, we must reserve additional addition
    assert(balance_upper_bound_.is_in_range<uint128_t>());
    auto balance_upper_bound = balance_upper_bound_.get_capped<uint128_t>();
    auto old_add_lock = from_xdr(held_lock.asset_add_amount);
    assert(!integer_sub_overflow<int128_t>(new_transfer_amount, old_add_lock));

    int128_t marginal_upper_bound = new_transfer_amount - old_add_lock;
    assert(marginal_upper_bound >= 0);

    if (!integer_add_overflow<uint128_t>(balance_upper_bound, marginal_upper_bound)) {
      balance_upper_bound_ += marginal_upper_bound;
      assert(balance_upper_bound_.is_in_range<uint128_t>());
      return AssetBalanceLock{
        .asset_add_amount = to_xdr(new_transfer_amount),
        .asset_subtract_amount = held_lock.asset_subtract_amount,
      };
    }

    return std::nullopt;
  }

  // otherwise, reserve the additional subtraction
  assert(balance_lower_bound_.is_in_range<uint128_t>());
  if (from_xdr(held_lock.asset_subtract_amount) <= new_transfer_amount) {
    // no new lock required
    return held_lock;
  }

  auto balance_lower_bound = balance_lower_bound_.get_capped<uint128_t>();
  auto old_sub_lock = from_xdr(held_lock.asset_subtract_amount);
  assert(!integer_add_overflow<int128_t>(new_transfer_amount, old_sub_lock));

  int128_t marginal_lower_bound = new_transfer_amount - old_sub_lock;
  if (!integer_add_overflow<uint128_t>(balance_lower_bound, marginal_lower_bound)) {
    assert(marginal_lower_bound <= 0);
    balance_lower_bound_ += marginal_lower_bound;
    assert(balance_lower_bound_.is_in_range<uint128_t>());
    return AssetBalanceLock{
      .asset_add_amount = held_lock.asset_add_amount,
      .asset_subtract_amount = to_xdr(new_transfer_amount),
    };
  }
  return std::nullopt;
}

int128_t
ExclusiveLockedState::get_cost(StorageDelta const &delta,
                               const StorageItemWrapper *base_item)
{
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    return kBaseItemCost + bytestring_cost(delta.bytestring().size());
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
    return kBaseItemCost;
  case StorageDeltaType::kDeltaTypeDelete:
    return 0;
  case StorageDeltaType::kDeltaTypeRetrocausalQueue:
  {
    auto past_stats = [&] {
      if (delta.retrocausal_queue().consume) {
        return base_item->queue().get_stats_after_consume(
            delta.retrocausal_queue().consume->past_block_consume);
      }
      return base_item->queue().get_active_stats();
    }();

    int64_t new_count = delta.retrocausal_queue().insert.added_entries.size();
    int64_t consumed_count =
        (delta.retrocausal_queue().consume
             ? delta.retrocausal_queue().consume->current_block_consumed.size()
             : 0);
    auto current_block_stats =
        RetrocausalQueueStats{.active_block_count = (new_count > 0) ? 1U : 0U,
                              .active_size = new_count - consumed_count};
    return retrocausal_queue_cost(past_stats) +
           retrocausal_queue_cost(current_block_stats) + kBaseItemCost;
  }
  default:
    assert(false);
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
    std::shared_ptr<const StorageItemWrapper> previous_item) const
{
  if (refcount_ > 0) {
    return std::visit(
        [&](auto &arm) {
          if constexpr (requires(decltype(arm) &a) { a.to_item(); }) {
            return arm.to_item();
          }
          else if constexpr (requires(decltype(arm) &a,
                                      const StorageItemWrapper *b) {
                               a.to_item(b);
                             }) {
            return arm.to_item(previous_item.get());
          }
          else {
            return arm.to_item(previous_item);
          }
        },
        variant_);
  }
  return previous_item;
}

int128_t
RefcountedDeltaVariant::get_cost_after_commit(
    StorageDelta const &delta, const StorageItemWrapper *base_item)
{
  assert(refcount_ > 0);

  return std::visit(
      [&](auto const &args) -> int128_t {
        using args_t = std::decay_t<decltype(args)>;
        if constexpr (std::same_as<args_t, DeleteArgs>) {
          assert(delta.type() == StorageDeltaType::kDeltaTypeDelete);
          return 0;
        }
        else if constexpr (std::same_as<args_t, BytestringArgs>) {
          assert(delta.type() == StorageDeltaType::kDeltaTypeBytestringSet);
          return args.get_cost();
        }
        else if constexpr (std::same_as<args_t, NNIntSetAddArgs>) {
          assert(delta.type() ==
                 StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
          return args.get_cost();
        }
        else if constexpr (std::same_as<args_t, RetrocausalQueueArgs>) {
          assert(delta.type() == StorageDeltaType::kDeltaTypeRetrocausalQueue);
          return args.get_cost(delta.retrocausal_queue(), base_item);
        }
        else if constexpr (std::same_as<args_t, UnmodifiedState>) {
          throw std::runtime_error("impossible: we must have refcount_ > 0");
        }
        else {
          static_assert(
              std::same_as<args_t, ExclusiveLockedState>,
              "match will occur if called on an exclusive locked state");

          return args.get_cost(delta, base_item);
        }
      },
      variant_);
}

} // namespace hogd
