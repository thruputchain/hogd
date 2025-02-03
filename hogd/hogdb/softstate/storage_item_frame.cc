#include "hogd/hogdb/softstate/storage_item_frame.h"

#include <cassert>
#include <limits>
#include <optional>
#include <stdexcept>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/softstate/uint256_reservation.h"
#include "storage_item.hh"
#include "storage_lock.hh"
#include "types.hh"

namespace hogd {

StorageItemFrame::StorageItemFrame(
    AddressAndKey const &addrkey,
    std::optional<StorageItemWrapper> const &snapshot_state,
    std::atomic<uint32_t> *bad_state_counter)
  : addr_key_(addrkey), committed_item_(snapshot_state),
    variant_(bad_state_counter)
{}

std::optional<StorageItemWrapper>
StorageItemFrame::BytestringArgs::to_item(
    std::optional<StorageItemWrapper> const & /*unused*/) const
{
  return StorageItemWrapper(bytestring_set_);
}

std::optional<StorageItemWrapper>
StorageItemFrame::NNIntSetAddArgs::to_item(
    std::optional<StorageItemWrapper> const & /*unused*/) const
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

  return StorageItemWrapper(out);
}

namespace {
x23::Vec<QueueEntry>
make_block(std::set<QueueEntry> &entries)
{
  x23::Vec<QueueEntry> new_block;
  for (auto entry : entries) {
    new_block.emplace_back(std::move(entry));
  }
  return new_block;
}
} // namespace

std::optional<StorageItemWrapper>
StorageItemFrame::RetrocausalQueueArgs::to_item(
    std::optional<StorageItemWrapper> const &previous_item)
{
  assert(!consumer_lock_);

  std::printf("RetrocausalQueueArgs::to_item\n");

  RetrocausalQueuePastBlockConsumeDelta consume_delta{
      .max_fully_consumed_block_number = 0, .partial_consumed_number = 0};
  if (past_consume_delta_) {
    assert(previous_item);
    assert(previous_item->type() == StorageItemType::kRetrocausalQueue);
    consume_delta = *past_consume_delta_;
  }

  if (previous_item.has_value() &&
      previous_item->type() == StorageItemType::kRetrocausalQueue) {
    uint64_t new_block_number =
        previous_item->queue().get_current_block_number() + 1;
    x23::Vec<QueueEntry> new_block = make_block(persistable_entries_);
    if (new_block.size() > 0) {
      return StorageItemWrapper(
          RetrocausalQueue(previous_item->queue(), consume_delta,
                           RetrocausalQueueBlock{.entries = new_block,
                                                 .block_number =
                                                     new_block_number}));
    }
    return StorageItemWrapper(
        RetrocausalQueue(previous_item->queue(), consume_delta));
  }
  // otherwise, no parent, so make empty one just for the ctor
  RetrocausalQueue fake_parent(0);
  assert(!past_consume_delta_);

  x23::Vec<QueueEntry> new_block = make_block(persistable_entries_);
  std::printf("RetrocausalQueueArgs::to_item: new_block.size() %ld\n",
              new_block.size());
  if (new_block.size() > 0) {
    std::printf("RetrocausalQueueArgs::to_item: returning new_block\n");
    return StorageItemWrapper(
        RetrocausalQueue(fake_parent, consume_delta,
                         {.entries = new_block, .block_number = 1}));
  }
  return StorageItemWrapper(RetrocausalQueue(fake_parent, consume_delta));
}

template<typename VariantArm, typename... VariantArgs>
VariantArm *
StorageItemFrame::RefcountedVariant::try_set_variant(VariantArgs const &...args)
{
  if (refcount_ == 0) {
    assert(std::holds_alternative<DeleteArgs>(variant_));
    variant_.emplace<VariantArm>(args...);
    return &std::get<VariantArm>(variant_);
  }

  if (!std::holds_alternative<VariantArm>(variant_)) {
    return nullptr;
  }

  if (!std::get<VariantArm>(variant_).typeclass_match(args...)) {
    return nullptr;
  }

  return &std::get<VariantArm>(variant_);
}

bool
StorageItemFrame::RefcountedVariant::try_reserve_bytestring_set(
    Bytestring const &bytestring_set)
{
  if ([[maybe_unused]] auto *args =
          try_set_variant<BytestringArgs>(bytestring_set)) {

    assert(args->bytestring_set_ == bytestring_set);
    acquire_reference();
    return true;
  }
  return false;
}

bool
StorageItemFrame::RefcountedVariant::try_reserve_delete()
{
  if ([[maybe_unused]] auto *args = try_set_variant<DeleteArgs>()) {
    acquire_reference();
    return true;
  }
  return false;
}

bool
StorageItemFrame::RefcountedVariant::try_reserve_retrocausal_queue_delta(
    bool requires_consume)
{
  if ([[maybe_unused]] auto *args = try_set_variant<RetrocausalQueueArgs>()) {
    if (requires_consume) {
      if (args->consume_has_committed_) {
        return false;
      }
      if (args->consumer_lock_) {
        return false;
      }
      args->consumer_lock_ = true;
    }
    acquire_reference();
    return true;
  }
  return false;
}

bool
StorageItemFrame::RefcountedVariant::try_reserve_nnint_set_add(
    int128_t set_value, int128_t add_value)
{
  if (try_set_variant<NNIntSetAddArgs>(set_value)) {
    acquire_reference();
    if (add_value >= 0) {
      // modifying total_added_ is deferred to commit time
      return true;
    }

    // if set_value <=0 and add_value < 0, delta always fails
    if (set_value <= 0) {
      release_reference();
      return false;
    }

    // the interesting case;
    // set_value > 0, add_value < 0
    auto res = try_reserve_nnint_additional(add_value);
    if (!res) {
      release_reference();
      return false;
    }
    return true;
  }
  return false;
}

// Does not change references
bool
StorageItemFrame::RefcountedVariant::try_reserve_nnint_additional(
    int128_t add_value)
{
  auto &args = get_variant_arm_unchecked<NNIntSetAddArgs>();
  assert(add_value < 0);

  int128_t desired_subtracted = 0;

  if (integer_add_overflow(args.total_subtracted_, add_value,
                           &desired_subtracted)) {
    // more than INT128_MIN is subtracted from the counter.
    // This is impossible, if we want to maintain that set+add >= 0.

    return false;
  }

  // desired_subtracted < 0, set_value > 0 => no overflow on addition
  if (desired_subtracted + args.set_value_ < 0) {
    // this new subtraction would drive the value below 0
    return false;
  }

  args.total_subtracted_ = desired_subtracted;
  return true;
}

void
StorageItemFrame::RefcountedVariant::commit_nnint_add(int128_t add_value)
{
  auto &args = get_variant_arm_unchecked<NNIntSetAddArgs>();
  assert(refcount_ > 0);
  if (add_value < 0) {
    return;
  }

  args.total_added_ += add_value;
}

void
StorageItemFrame::RefcountedVariant::revert_nnint_set_add(int128_t add_value)
{
  assert(refcount_ > 0);
  auto &args = get_variant_arm_unchecked<NNIntSetAddArgs>();

  if (add_value >= 0) {
    args.total_added_ -= add_value;
  }
  else {
    args.total_subtracted_ -= add_value;
  }
}

void
StorageItemFrame::RefcountedVariant::commit_retrocausal_queue_modify(
    RetrocausalQueueDelta const &queue_delta)
{
  assert(std::holds_alternative<RetrocausalQueueArgs>(variant_));
  assert(refcount_ > 0);
  auto &args = get_variant_arm_unchecked<RetrocausalQueueArgs>();

  for (auto const &entry : queue_delta.insert.added_entries) {
    auto it = args.consumed_but_not_inserted_entries_.find(entry);
    if (it == args.consumed_but_not_inserted_entries_.end()) {
      args.persistable_entries_.insert(entry);
    }
    else {
      args.consumed_but_not_inserted_entries_.erase(it);
      if (args.consumed_but_not_inserted_entries_.empty()) {
        unmark_as_in_bad_state();
      }
    }
    args.new_entries_.push_back(entry);
  }

  if (queue_delta.consume) {
    assert(!args.consume_has_committed_);
    // not strictly necessary, but we maintain the invariant that at the end of
    // the block consumer_lock_ is released, just to be careful.
    if (args.consumer_lock_) {
      args.consumer_lock_ = false;
    }
    args.consume_has_committed_ = true;
    if (queue_delta.consume->past_block_consume !=
        RetrocausalQueuePastBlockConsumeDelta{.max_fully_consumed_block_number =
                                                  0,
                                              .partial_consumed_number = 0}) {
      args.past_consume_delta_.emplace(queue_delta.consume->past_block_consume);
    }

    for (auto const &consumed_entry :
         queue_delta.consume->current_block_consumed) {
      auto res = args.persistable_entries_.erase(consumed_entry);
      if (res == 0) {
        if (args.consumed_but_not_inserted_entries_.empty()) {
          mark_as_in_bad_state();
        }
        args.consumed_but_not_inserted_entries_.insert(consumed_entry);
      }
    }
  }
}

bool
StorageItemFrame::RefcountedVariant::matches_typeclass(
    StorageItemTypeClass typeclass) const
{
  assert(refcount_ > 0);

  return std::visit(
      [&typeclass](auto const &args) {
        if constexpr (std::same_as<std::decay_t<decltype(args)>, DeleteArgs>) {
          return typeclass.item_type() == StorageItemType::kDeleted &&
                 args.typeclass_match();
        }
        else if constexpr (std::same_as<std::decay_t<decltype(args)>,
                                        BytestringArgs>) {
          return typeclass.item_type() == StorageItemType::kBytestring &&
                 args.typeclass_match(typeclass.bytestring());
        }
        else if constexpr (std::same_as<std::decay_t<decltype(args)>,
                                        NNIntSetAddArgs>) {
          return typeclass.item_type() == StorageItemType::kNonnegativeInt &&
                 args.typeclass_match(from_xdr(typeclass.nnint_set()));
        }
        else if constexpr (std::same_as<std::decay_t<decltype(args)>,
                                        RetrocausalQueueArgs>) {
          return typeclass.item_type() == StorageItemType::kRetrocausalQueue &&
                 args.typeclass_match();
        }
        else {
          static_assert(
              std::same_as<std::decay_t<decltype(args)>, ExclusiveLockedState>,
              "should never match exclusive locked state");
          return false;
        }
      },
      variant_);
}

bool
StorageItemFrame::try_reserve_delta(StorageDelta const &delta)
{
  assert(!variant_.permanent_exclusive_lock_);
  switch (delta.type()) {
  case StorageDeltaType::kBytestringSet:
    return variant_.try_reserve_bytestring_set(delta.bytestring());
  case StorageDeltaType::kNonnegativeIntSetAdd:
    return variant_.try_reserve_nnint_set_add(from_xdr(delta.nnint().set_value),
                                              from_xdr(
                                                  delta.nnint().add_value));
  case StorageDeltaType::kDelete:
    return variant_.try_reserve_delete();
  case StorageDeltaType::kRetrocausalQueueModify:
    return variant_.try_reserve_retrocausal_queue_delta(
        delta.retrocausal_queue().consume != nullptr);
  default:
    throw std::runtime_error("unknown delta type");
  }
}

void
StorageItemFrame::commit_delta(StorageDelta const &delta,
                               bool maintain_permanent_exclusive_lock)
{
  switch (delta.type()) {
  case StorageDeltaType::kBytestringSet:
    break;
  case StorageDeltaType::kNonnegativeIntSetAdd:
    variant_.commit_nnint_add(from_xdr(delta.nnint().add_value));
    break;
  case StorageDeltaType::kDelete:
    break;
  case StorageDeltaType::kRetrocausalQueueModify:
    variant_.commit_retrocausal_queue_modify(delta.retrocausal_queue());
    break;
  default:
    throw std::runtime_error("unimpl");
  }

  if (maintain_permanent_exclusive_lock) {
    variant_.permanent_exclusive_lock_ = true;
  }
}

std::optional<StorageItemWrapper> const &
StorageItemFrame::get_snapshot_item() const
{
  assert(!round_committed_);
  return committed_item_;
}

std::optional<StorageItemWrapper> const &
StorageItemFrame::get_post_close_block_item()
{
  update_committed_item();
  return committed_item_;
}

void
StorageItemFrame::update_committed_item()
{
  if (!round_committed_) {
    std::printf("update_committed_item: refcount %d\n", variant_.refcount_);
    if (variant_.refcount_ > 0) {
      committed_item_ =
          std::visit([&](auto &arm) { return arm.to_item(committed_item_); },
                     variant_.variant_);
    }
    round_committed_ = true;
  }
}

bool
StorageItemFrame::try_acquire_exclusive_lock(StorageLockType previous_lock_type)
{
  assert(!round_committed_);
  if (variant_.permanent_exclusive_lock_) {
    assert(variant_.refcount_ > 0);
    return false;
  }
  if (variant_.refcount_ == 0) {
    assert(previous_lock_type == StorageLockType::kNone);
    variant_.variant_ = ExclusiveLockedState{};
    variant_.acquire_reference();
    return true;
  }

  switch (previous_lock_type) {
  case StorageLockType::kNone:
  {
    if (variant_.refcount_ == 0) {
      assert(!variant_.permanent_exclusive_lock_);
      // no references outstanding, so we can acquire the exclusive lock
      variant_.variant_ = ExclusiveLockedState{};
      variant_.acquire_reference();
      return true;
    }
    return false;
  }
  case StorageLockType::kSetItemTypeClass:
  {
    // do we have the only exclusive lock?
    bool res = (variant_.refcount_ == 1);
    assert(!variant_.permanent_exclusive_lock_);
    assert(!std::holds_alternative<ExclusiveLockedState>(variant_.variant_));

    if (res) {
      variant_.variant_ = ExclusiveLockedState{};
      // no need to acquire an additional reference, as we already have one
      return true;
    }
    return false;
  }
  case StorageLockType::kExclusiveLock:
  case StorageLockType::kPermanentExclusiveLock:
    // we already have the exclusive lock, so we can return true
    // This technically seems like acquiring the lock that we already have
    // but this lets the external logic be agnostic to whether we already have
    // the lock or not.
    return true;
  }

  assert(false);
  throw std::runtime_error("unreachable");
}

void
StorageItemFrame::revert_exclusive_lock_to_none()
{
  variant_.release_reference();
  assert(variant_.refcount_ == 0);
}

void
StorageItemFrame::revert_exclusive_lock_to_set_typeclass(
    StorageDelta const &delta)
{
  assert(variant_.refcount_ == 1);
  variant_.release_reference();
  // refcount is 0, try_reserve_delta thinks this is an unmodified item now
  [[maybe_unused]]
  bool res = try_reserve_delta(delta); // reacquires a reference
  assert(res);
  assert(variant_.refcount_ == 1);
}

void
StorageItemFrame::revert_set_typeclass_lock(
    TypeSpecificLock const &type_specific_lock)
{
  switch (type_specific_lock.item_type()) {
  case StorageItemType::kNonnegativeInt:
    variant_.revert_nnint_set_add(
        from_xdr(type_specific_lock.nnint_subtracted_amount()));
    break;
  case StorageItemType::kRetrocausalQueue:
    if (type_specific_lock.consume_lock() == 1) {
      variant_.release_retrocausal_queue_consume();
    }
    break;
  default:
    break;
  }

  variant_.release_reference();
}

bool
StorageItemFrame::RefcountedVariant::try_reserve_retrocausal_queue_consume()
{
  assert(std::holds_alternative<RetrocausalQueueArgs>(variant_));
  assert(refcount_ > 0);
  auto &args = get_variant_arm_unchecked<RetrocausalQueueArgs>();

  if (args.consumer_lock_) {
    return false;
  }
  // a consume delta already committed
  if (args.past_consume_delta_) {
    return false;
  }

  args.consumer_lock_ = true;
  return true;
}

void
StorageItemFrame::RefcountedVariant::release_retrocausal_queue_consume()
{
  assert(std::holds_alternative<RetrocausalQueueArgs>(variant_));
  assert(refcount_ > 0);
  auto &args = get_variant_arm_unchecked<RetrocausalQueueArgs>();
  assert(args.consumer_lock_);
  args.consumer_lock_ = false;
}

namespace {

StorageItemTypeClass
to_typeclass(StorageDelta const &delta)
{
  switch (delta.type()) {
  case StorageDeltaType::kBytestringSet:
  {
    auto out = StorageItemTypeClass(StorageItemType::kBytestring);
    out.bytestring() = delta.bytestring();
    return out;
  }
  case StorageDeltaType::kNonnegativeIntSetAdd:
  {
    auto out = StorageItemTypeClass(StorageItemType::kNonnegativeInt);
    out.nnint_set() = delta.nnint().set_value;
    return out;
  }
  case StorageDeltaType::kDelete:
    return StorageItemTypeClass(StorageItemType::kDeleted);
  case StorageDeltaType::kRetrocausalQueueModify:
    return StorageItemTypeClass(StorageItemType::kRetrocausalQueue);
  default:
    throw std::runtime_error("unimplemented");
  }
  // unreachable
}

StorageLock
to_lock(StorageDelta const &delta)
{
  StorageLock lock(StorageLockType::kSetItemTypeClass);
  lock.full_typeclass_lock().typeclass = to_typeclass(delta);
  switch (delta.type()) {
  case StorageDeltaType::kBytestringSet:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kBytestring);
    return lock;
  case StorageDeltaType::kNonnegativeIntSetAdd:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kNonnegativeInt);
    lock.full_typeclass_lock().extra_lock_data.nnint_subtracted_amount() =
        to_xdr(std::min<int128_t>(0, from_xdr(delta.nnint().add_value)));
    return lock;
  case StorageDeltaType::kRetrocausalQueueModify:
  {
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kRetrocausalQueue);
    if (delta.retrocausal_queue().consume) {
      lock.full_typeclass_lock().extra_lock_data.consume_lock() = 1;
    }
    return lock;
  }
  case StorageDeltaType::kDelete:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kDeleted);
    return lock;
  default:
    return lock;
  }
}

} // namespace

std::optional<StorageLock>
StorageItemFrame::try_reserve(StorageDelta const &delta,
                              StorageLock const &previous_lock)
{
  assert(!round_committed_);
  switch (previous_lock.lock_type()) {
  case StorageLockType::kNone:
  {
    if (variant_.permanent_exclusive_lock_) {
      return std::nullopt;
    }
    if (try_reserve_delta(delta)) {
      return to_lock(delta);
    }
    return std::nullopt;
  }
  case StorageLockType::kSetItemTypeClass:
  {
    assert(variant_.refcount_ > 0);
    assert(!variant_.permanent_exclusive_lock_);
    auto req_lock = to_lock(delta);
    assert(req_lock.lock_type() == StorageLockType::kSetItemTypeClass);

    if (req_lock.full_typeclass_lock().typeclass !=
        previous_lock.full_typeclass_lock().typeclass) {
      // upgrade to exclusive
      if (!try_acquire_exclusive_lock(StorageLockType::kSetItemTypeClass)) {
        return std::nullopt;
      }
      return StorageLock(StorageLockType::kExclusiveLock);
    }

    assert(variant_.matches_typeclass(
        previous_lock.full_typeclass_lock().typeclass));
    assert(
        variant_.matches_typeclass(req_lock.full_typeclass_lock().typeclass));

    switch (req_lock.full_typeclass_lock().extra_lock_data.item_type()) {
    case StorageItemType::kNonnegativeInt:
    {
      int128_t prev_sub_amount =
          from_xdr(previous_lock.full_typeclass_lock()
                       .extra_lock_data.nnint_subtracted_amount());
      int128_t req_sub_amount =
          from_xdr(req_lock.full_typeclass_lock()
                       .extra_lock_data.nnint_subtracted_amount());

      assert(prev_sub_amount <= 0);
      assert(req_sub_amount <= 0);
      assert(prev_sub_amount != std::numeric_limits<int128_t>::min());
      if (prev_sub_amount <= req_sub_amount) {
        return previous_lock;
      }

      auto reserve_res = variant_.try_reserve_nnint_additional(req_sub_amount -
                                                               prev_sub_amount);
      if (!reserve_res) {
        return std::nullopt;
      }

      return req_lock;
    } break;
    case StorageItemType::kRetrocausalQueue:
    {
      if (req_lock.full_typeclass_lock().extra_lock_data.consume_lock() == 1) {
        if (variant_.try_reserve_retrocausal_queue_consume()) {
          return req_lock;
        }
        return std::nullopt;
      }
      assert(req_lock == previous_lock);
      return previous_lock;
    }
    default:
      return previous_lock;
    }
  }
  // always succeeds if we have an exclusive lock
  case StorageLockType::kExclusiveLock:
  case StorageLockType::kPermanentExclusiveLock:
    return previous_lock;
  default:
    throw std::runtime_error("impossible");
  }
}

void
StorageItemFrame::revert_set_typeclass_lock_to_set_typeclass(
    StorageLock const &lock, StorageDelta const &delta)
{
  assert(lock.lock_type() == StorageLockType::kSetItemTypeClass);
  switch (delta.type()) {
  case StorageDeltaType::kBytestringSet:
    // nothing to do
    break;
  case StorageDeltaType::kNonnegativeIntSetAdd:
  {
    assert(lock.full_typeclass_lock().extra_lock_data.item_type() ==
           StorageItemType::kNonnegativeInt);
    assert(delta.type() == StorageDeltaType::kNonnegativeIntSetAdd);

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
      variant_.revert_nnint_set_add(diff);
    }
    break;
  }
  case StorageDeltaType::kDelete:
    // nothing to do
    break;
  case StorageDeltaType::kRetrocausalQueueModify:
  {
    // If we took out the consume lock, but no longer need it
    if (lock.full_typeclass_lock().extra_lock_data.consume_lock() == 1) {
      if (!delta.retrocausal_queue().consume) {
        variant_.release_retrocausal_queue_consume();
      }
    }
    break;
  }
  default:
    throw std::runtime_error("unimplemented");
  }
}

void
StorageItemFrame::commit(StorageDelta const &delta, StorageLock const &lock)
{
  assert(!round_committed_);
  assert(lock.lock_type() != StorageLockType::kNone);
  switch (lock.lock_type()) {
  case StorageLockType::kSetItemTypeClass:
    revert_set_typeclass_lock_to_set_typeclass(lock, delta);
    commit_delta(delta, false);
    return;
  case StorageLockType::kExclusiveLock:
    revert_exclusive_lock_to_set_typeclass(delta);
    commit_delta(delta, false);
    return;
  case StorageLockType::kPermanentExclusiveLock:
    revert_exclusive_lock_to_set_typeclass(delta);
    commit_delta(delta, true);
    return;
  case StorageLockType::kNone:
    assert(false);
  default:
    throw std::runtime_error("impossible");
  }
}

void
StorageItemFrame::revert(StorageLock const &lock)
{
  assert(!round_committed_);
  [[maybe_unused]]
  auto initial_refcount = variant_.refcount_;
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

  assert(variant_.refcount_ == initial_refcount - 1);
}

std::optional<QueueEntry>
StorageItemFrame::get_concurrent_retrocausal_queue_entry(uint32_t index)
{
  // This is a read method, not a write method.  Therefore, this does not check
  // for locks held by this or other threads (including the
  // permanent_exclusive_lock_).

  if (!std::holds_alternative<RetrocausalQueueArgs>(variant_.variant_)) {
    return std::nullopt;
  }

  // if refcount is 0, then this should be a DeletedArgs item, not a
  // RetrocausalQueueArgs.
  assert(variant_.refcount_ > 0);
  return variant_.get_concurrent_retrocausal_queue_entry(index);
}

std::optional<QueueEntry>
StorageItemFrame::RefcountedVariant::get_concurrent_retrocausal_queue_entry(
    uint32_t index)
{
  assert(std::holds_alternative<RetrocausalQueueArgs>(variant_));
  assert(refcount_ > 0);
  auto &args = get_variant_arm_unchecked<RetrocausalQueueArgs>();

  if (index >= args.new_entries_.size()) {
    return std::nullopt;
  }

  return args.new_entries_[index];
}

} // namespace hogd
