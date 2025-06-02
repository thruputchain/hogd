#include "hogd/storage_item/refcounted_delta_variant.h"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <memory>
#include <optional>
#include <print>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/invalid_state_flag.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "x23/vec.h"

namespace hogd {

std::shared_ptr<const StorageItemWrapper>
BytestringArgs::to_item(StorageCostModel const* cost_model) const
{
  assert(committed_);
  return std::make_shared<const StorageItemWrapper>(bytestring_set_, cost_model);
}

std::shared_ptr<const StorageItemWrapper>
NNIntSetAddArgs::to_item(StorageCostModel const* cost_model) const
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
RetrocausalQueueArgs::to_item(const StorageItemWrapper *previous_item, StorageCostModel const* cost_model) const
{
  assert(!consumer_lock_);

  RetrocausalQueuePastBlockConsumeDelta consume_delta = kEmptyConsume;
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
    if (!new_block.empty()) {
      return std::make_shared<const StorageItemWrapper>(
          RetrocausalQueue(parent, consume_delta,
                           RetrocausalQueueBlock{.entries = new_block,
                                                 .block_number =
                                                     new_block_number}), cost_model);
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
RetrocausalQueueArgs::get_current_committed_cost(StorageCostModel const* current_model, StorageItemWrapper const* base_item) const
{
  auto stats = get_current_committed_stats(base_item);
  return current_model->kBaseItemCost + current_model->get_retrocausal_queue_cost(stats.active_size, stats.active_block_count);
}

int128_t
RetrocausalQueueArgs::get_marginal_cost_upper_bound(StorageDelta const& delta, StorageCostModel const* current_model, StorageItemWrapper const* base_item) const
{
  assert(delta.type() == StorageDeltaType::kDeltaTypeRetrocausalQueue);

  // TODO(geoff): Because the queue is single-consumer,
  // we can grant a refund to the consumer immediately;
  // This doesn't mean returning a negative cost, but rather
  // that a delta that consumes 100 entries and
  // creates 100 entries can have a net cost of 0.
  // this requires checking over the past stats, which we have not implemented yet here.
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
        return queue.get_stats_after_consume(delta.retrocausal_queue().consume->past_block_consume);
      }
      return queue.get_active_stats();
    }
    return RetrocausalQueueStats{.active_block_count = 0, .active_size = 0};
  }(); */

  int64_t new_inserted_entries = delta.retrocausal_queue().insert.added_entries.size();
  if (delta.retrocausal_queue().consume) {
    new_inserted_entries -= delta.retrocausal_queue().consume->current_block_consumed.size();
  }

  // We can only create a new block if we insert entries

  bool new_block_maybe_created = !delta.retrocausal_queue().insert.added_entries.empty();

  RetrocausalQueueStats new_stats = {
      .active_block_count = new_block_maybe_created ? 1U : 0U,
      .active_size = std::max<int64_t>(0, new_inserted_entries)};

  int128_t base_cost = (base_item || insert_committed_ || past_consume_delta_) ? 0 : current_model->kBaseItemCost;

  return base_cost + current_model->get_retrocausal_queue_cost(new_stats.active_size, new_stats.active_block_count);
}

RetrocausalQueueStats
RetrocausalQueueArgs::get_current_committed_stats(StorageItemWrapper const* base_item) const
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

  int64_t current_block_size = persistable_entries_.size() - consumed_but_not_inserted_entries_.size();

  RetrocausalQueueStats new_stats = {
      .active_block_count =
          (persistable_entries_.empty()) ? 0U : 1U,
      .active_size = current_block_size};

  return RetrocausalQueueStats{
      .active_block_count = past_stats.active_block_count + new_stats.active_block_count,
      .active_size = past_stats.active_size + new_stats.active_size};
}

/*
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
} */

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

/*
int128_t
ExclusiveLockedState::get_cost_after_commit(StorageDelta const &delta,
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
} */

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
    std::shared_ptr<const StorageItemWrapper> previous_item, StorageCostModel const* cost_model) const
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
          else if constexpr (requires(decltype(arm) &a,
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
RefcountedDeltaVariant::get_current_committed_cost(StorageCostModel const* current_model, StorageItemWrapper const* base_item) const
{
  return std::visit(
      [&](auto const &args) { return args.get_current_committed_cost(current_model, base_item); },
      variant_);
}


int128_t
RefcountedDeltaVariant::get_marginal_cost_upper_bound(
    StorageDelta const& delta, 
    StorageCostModel const* current_model, 
    StorageItemWrapper const* base_item) const
{
  assert(refcount_ > 0);
  int128_t out = std::visit(
      [&](auto const &args) { return args.get_marginal_cost_upper_bound(delta, current_model, base_item); },
      variant_);
  assert(out >= 0);
  return out;
}

} // namespace hogd
