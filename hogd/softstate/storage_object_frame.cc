#include "hogd/softstate/storage_object_frame.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/softstate/uint128_reservation.h"
#include "storage_object.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

std::optional<StorageObject>
StorageObjectFrame::NNInt64SetAddArgs::to_object(
    std::optional<StorageObject> const & /*unused*/) const
{
  StorageObject object;
  object.type(StorageObjectType::kNonnegativeInt64);
  object.int64() = set_value_;

  object.int64() += total_subtracted_;

  uint64_t total_add = total_added_.get_capped();

  if (integer_add_overflow<int64_t>(object.int64(), total_add)) {
    object.int64() = INT64_MAX;
  }
  else {
    object.int64() += total_add;
  }

  return object;
}

std::optional<StorageObject>
StorageObjectFrame::ReplayCacheArgs::to_object(
    std::optional<StorageObject> const &previous_object) const
{
  StorageObject out;
  if (previous_object) {
    out = *previous_object;
  }
  else {
    out.type(StorageObjectType::kReplayCache);
    out.replay_cache().metadata.clear_threshold = 0;
  }

  // TODO(geoff): this is a very inefficient but correct way of doing the data
  // structures.  This should be a set or a BTree
  for (auto val : new_nonces_) {
    out.replay_cache().nonces.push_back(val);
  }

  std::sort(out.replay_cache().nonces.begin(), out.replay_cache().nonces.end(),
            std::greater<>());

  if (new_max_clear_threshold_.has_value()) {

    assert(new_max_clear_threshold_.value() >
           out.replay_cache().metadata.clear_threshold);

    // TODO(geoff) lazy - swap for a binary search
    for (uint32_t i = 0; i < out.replay_cache().nonces.size(); i++) {
      if (out.replay_cache().nonces[i] < new_max_clear_threshold_.value()) {
        out.replay_cache().nonces.resize(i);
        break;
      }
    }

    out.replay_cache().metadata.clear_threshold = *new_max_clear_threshold_;
  }

  out.replay_cache().metadata.current_size = out.replay_cache().nonces.size();

  return out;
}

std::optional<StorageObject>
StorageObjectFrame::MessageQueueArgs::to_object(
    std::optional<StorageObject> const &previous_object) const
{
  // TODO(geoff) inefficient but correcg way of doing this data structure.

  StorageObject out;
  out.type(StorageObjectType::kMessageQueue);

  for (auto const &event : new_events_) {
    out.message_queue().events.push_back(event);
  }

  if (previous_object) {
    out.message_queue().events.append(
        previous_object->message_queue().events.begin(),
        previous_object->message_queue().events.end());
  }

  uint32_t size = out.message_queue().events.size();
  assert(size >= num_messages_consumed_);

  out.message_queue().events.resize(size - num_messages_consumed_);

  return out;
}

template<typename VariantArm, typename... VariantArgs>
VariantArm *
StorageObjectFrame::try_set_variant(RefcountedVariant &variant,
                                    VariantArgs const &...args)
{
  if (variant.refcount_ == 0) {
    variant.variant_.emplace<VariantArm>(args...);
  }

  if (!std::holds_alternative<VariantArm>(variant.variant_)) {
    return nullptr;
  }

  if (!std::get<VariantArm>(variant.variant_).can_accept(args...)) {
    return nullptr;
  }

  return &std::get<VariantArm>(variant.variant_);
}

template<typename VariantArm, typename... VariantArgs>
void
StorageObjectFrame::set_variant_from_exclusive_lock(RefcountedVariant &variant,
                                                    VariantArgs const &...args)
{
  assert(std::holds_alternative<ExclusiveLockedArgs>(variant.variant_));

  variant.variant_.emplace<VariantArm>(args...);

  assert(variant.refcount_ == 1);
}

StorageObjectFrame::StorageObjectFrame(
    std::optional<StorageObject> const &snapshot_state)
  : committed_base_object_(snapshot_state)
{}

StorageReserveResult
StorageObjectFrame::try_reserve_bytestring_set(
    RefcountedVariant &v, Bytestring const &bytestring_set,
    std::optional<StorageLock> const &lock)
{
  if ([[maybe_unused]] auto *args =
          try_set_variant<BytestringArgs>(v, bytestring_set)) {

    assert(args->bytestring_set_ == bytestring_set);

    if (!lock.has_value()) {
      acquire_variant_reference(v);
    }

    return StorageReserveResult::kSuccess;
  }
  return StorageReserveResult::kDefiniteFailure;
}

StorageReserveResult
StorageObjectFrame::try_reserve_nnint64_set_add(
    RefcountedVariant &v, int64_t set_value, int64_t add_value,
    std::optional<StorageLock> &lock)
{

  if (auto *args = try_set_variant<NNInt64SetAddArgs>(v, set_value)) {
    auto release_lock = [&](bool success) {
      if (!lock.has_value()) {
        // we have as yet no reference on the typeclass
        if (success) {
          acquire_variant_reference(v);
        }
      }
      else {

        // release any reserved subtraction
        // already has a variant reference, so release if reservation fails

        if (!success) {
          release_variant_reference(v);
        }

        if (lock->lock_type() == StorageLockType::kSetObjectTypeClass) {
          assert(lock->extra_lock_data().object_type() ==
                 StorageObjectType::kNonnegativeInt64);

          int64_t locked_subtract =
              lock->extra_lock_data().nnint64_subtracted_amount();

          assert(locked_subtract <= 0);
          assert(args->total_subtracted_ <= locked_subtract);
          args->total_subtracted_ -= locked_subtract;
        }
        // nothing else to do for an exclusive lock
      }
    };

    assert(args->set_value_ == set_value);

    if (add_value >= 0) {
      args->total_added_ += add_value;

      release_lock(true);

      return StorageReserveResult::kSuccess;
    }

    if (set_value < 0) { // and add_value < 0

      release_lock(false);

      return StorageReserveResult::kDefiniteFailure;
    }

    int64_t desired_subtracted = 0;

    // add_value < 0, and set_value >= 0

    int64_t marginal_reserve = add_value;

    if (lock.has_value()) {

      if (lock->lock_type() == StorageLockType::kExclusiveLock) {
        assert(args->total_subtracted_ == 0);

        assert(set_value + add_value >= 0);

        args->total_subtracted_ = add_value;

        release_lock(true);

        return StorageReserveResult::kSuccess;
      }

      assert(lock->lock_type() == StorageLockType::kSetObjectTypeClass);

      int64_t reserved_sub =
          lock->extra_lock_data().nnint64_subtracted_amount();

      assert(reserved_sub <= 0);

      marginal_reserve = add_value - reserved_sub;

      if (marginal_reserve >= 0) {
        // everything has been locked already

        // setting so that release_lock() operates correctly
        // remaining locked amount: reserved_sub - add_value
        lock->extra_lock_data().nnint64_subtracted_amount() = -marginal_reserve;
        // no need to modify args -> total_subtracted, as release_lock() will
        // handle that

        release_lock(true);

        return StorageReserveResult::kSuccess;
      }

      // Otherwise, must reserve the remaining amount
    }

    if (integer_add_overflow(args->total_subtracted_, marginal_reserve,
                             &desired_subtracted)) {

      // reserving the remaining amount failed
      release_lock(false);
      return StorageReserveResult::kDefiniteFailure;
    }

    // Cannot overflow.
    // set_value > 0 and desired_sub < 0.  So worst case
    // we get 1 + INT64_MIN, which does not overflow.
    if (set_value + desired_subtracted < 0) {

      release_lock(false);
      return StorageReserveResult::kDefiniteFailure;
    }

    args->total_subtracted_ = desired_subtracted;

    if (lock.has_value() &&
        lock->lock_type() == StorageLockType::kSetObjectTypeClass) {
      lock->extra_lock_data().nnint64_subtracted_amount() = 0;
    }

    release_lock(true);

    return StorageReserveResult::kSuccess;
  }

  assert(!lock.has_value());

  return StorageReserveResult::kDefiniteFailure;
}

void
StorageObjectFrame::revert_nnint64_set_add(RefcountedVariant &v,
                                           int64_t add_value)
{
  auto &args = get_variant_arm_unchecked<NNInt64SetAddArgs>(v);

  if (add_value >= 0) {
    args.total_added_ -= add_value;
  }
  else {
    args.total_subtracted_ -= add_value;
  }
}

/*
StorageReserveResult
StorageObjectFrame::try_reserve_replay_cache_inserts(
    x23::Vec<uint64_t> const &nonces)
{
  if (!type_guard(StorageObjectType::kReplayCache)) {
    return StorageReserveResult::kDefiniteFailure;
  }

  if (committed_base_object_) {
    assert(committed_base_object_->type() == StorageObjectType::kReplayCache);

    for (auto nonce : nonces) {
      if (committed_base_object_->replay_cache().metadata.clear_threshold >
          nonce) {
        return StorageReserveResult::kDefiniteFailure;
      }
      for (auto v : committed_base_object_->replay_cache().nonces) {
        if (v == nonce) {
          return StorageReserveResult::kDefiniteFailure;
        }
      }
    }
  }

  auto v = variant_.take();

  if (auto *args = try_set_variant<ReplayCacheArgs>(*v)) {

    for (auto nonce : nonces) {
      if (args->new_nonces_.contains(nonce)) {
        release_variant(*v);
        return StorageReserveResult::kDefiniteFailure;
      }
    }

    for (auto nonce : nonces) {
      args->new_nonces_.insert(nonce);
    }

    return StorageReserveResult::kSuccess;
  }

  return StorageReserveResult::kDefiniteFailure;
}

void
StorageObjectFrame::revert_replay_cache_inserts(
    x23::Vec<uint64_t> const &nonces)
{
  auto v = variant_.take();
  auto &args = get_variant_arm_unchecked<ReplayCacheArgs>(*v);

  for (auto nonce : nonces) {
    [[maybe_unused]] auto res = args.new_nonces_.erase(nonce);
    assert(res == 1);
  }

  release_variant(*v);
}

StorageReserveResult
StorageObjectFrame::try_reserve_replay_cache_clear(uint64_t threshold)
{
  if (!type_guard(StorageObjectType::kReplayCache)) {
    return StorageReserveResult::kDefiniteFailure;
  }

  if (committed_base_object_) {
    assert(committed_base_object_->type() == StorageObjectType::kReplayCache);

    if (committed_base_object_->replay_cache().metadata.clear_threshold >
        threshold) {
      return StorageReserveResult::kDefiniteFailure;
    }
  }

  auto v = variant_.take();

  if (try_set_variant<ReplayCacheArgs>(*v)) {

    return StorageReserveResult::kSuccess;
  }

  return StorageReserveResult::kDefiniteFailure;
}

void
StorageObjectFrame::commit_replay_cache_clear(uint64_t threshold)
{
  auto v = variant_.take();
  auto &args = get_variant_arm_unchecked<ReplayCacheArgs>(*v);

  if (args.new_max_clear_threshold_.has_value()) {
    args.new_max_clear_threshold_ =
        std::max(threshold, *args.new_max_clear_threshold_);
  }
  else {
    args.new_max_clear_threshold_ = threshold;
  }
}

void
StorageObjectFrame::revert_replay_cache_clear()
{
  auto v = variant_.take();
  release_variant(*v);
}

StorageReserveResult
StorageObjectFrame::try_reserve_mq_append(MessageQueueEvent const &event)
{
  if (!type_guard(StorageObjectType::kMessageQueue)) {
    return StorageReserveResult::kDefiniteFailure;
  }

  auto v = variant_.take();

  if (auto *args = try_set_variant<MessageQueueArgs>(*v)) {
    args->new_events_.insert(event);

    return StorageReserveResult::kSuccess;
  }

  return StorageReserveResult::kDefiniteFailure;
}

void
StorageObjectFrame::revert_mq_append(MessageQueueEvent const &event)
{
  auto v = variant_.take();
  auto &args = get_variant_arm_unchecked<MessageQueueArgs>(*v);

  [[maybe_unused]] auto res = args.new_events_.erase(event);
  assert(res == 1);

  release_variant(*v);
}

StorageReserveResult
StorageObjectFrame::try_reserve_mq_consume(uint32_t num_messages)
{
  if (!type_guard(StorageObjectType::kMessageQueue)) {
    return StorageReserveResult::kDefiniteFailure;
  }

  auto v = variant_.take();

  if (auto *args = try_set_variant<MessageQueueArgs>(*v)) {

    assert(num_messages != 0);

    if (args->num_messages_consumed_ == 0) {
      args->num_messages_consumed_ = num_messages;
      return StorageReserveResult::kSuccess;
    }

    return StorageReserveResult::kDefiniteFailure;
  }

  return StorageReserveResult::kDefiniteFailure;
}

void
StorageObjectFrame::revert_mq_consume()
{
  auto v = variant_.take();
  auto &args = get_variant_arm_unchecked<MessageQueueArgs>(*v);

  args.num_messages_consumed_ = 0;

  release_variant(*v);
}

*/

// no-op if return false
bool
StorageObjectFrame::perform_type_specific_lock_change(
    RefcountedVariant &v, TypeSpecificLock const &marginal_lock)
{
  switch (marginal_lock.object_type()) {
  case StorageObjectType::kNonnegativeInt64:
  {
    assert(v.refcount_ > 0);
    assert(std::holds_alternative<NNInt64SetAddArgs>(v));

    auto &args = get_variant_arm_unchecked<NNInt64SetAddArgs>(v);

    int64_t sub_delta = marginal_lock.nnint64_subtracted_amount();

    if (sub_delta >= 0) {
      // infallible operation
      args.total_subtracted_ += sub_delta;
      assert(args.total_subtracted_ <= 0);
      return true;
    }

    int64_t desired_subtracted = 0;
    if (integer_add_overflow(args.total_subtracted_, sub_delta,
                             &desired_subtracted)) {
      return false;
    }

    if (args.set_value_ < 0) {
      return false;
    }

    // args.set_value_ >= 0, desired_subtracted < 0 => cannot overflow

    if (args.set_value_ + desired_subtracted < 0) {
      return false;
    }
    args.total_subtracted_ = desired_subtracted;
    return true;
  } break;
  default:
    // nothing to do
    return true;
  }
}

StorageReserveResult
StorageObjectFrame::try_change_lock(StorageLockChange const &lock_change)
{
  auto v = variant_.take();

  if (lock_change.new_type == StorageLockType::kExclusiveLock) {
    assert(lock_change.previous_type != StorageLockType::kExclusiveLock);

    // if there is an active reference, it must be ours
    if (v->refcount_ == 0) {
      assert(lock_change.previous_type != StorageLockType::kSetObjectTypeClass);

      if (lock_change.previous_type == StorageLockType::kNone) {

        v->variant_.emplace<ExclusiveLockedArgs>();
        acquire_variant_reference(*v);

        return StorageReserveResult::kSuccess;
      }
      return StorageReserveResult::kDefiniteFailure;
    }

    if (v->refcount_ == 1) {
      if (lock_change.previous_type == StorageLockType::kNone) {
        // someone else has a reference to whatever the current typeclass is
        return StorageReserveResult::kDefiniteFailure;
      }

      assert(lock_change.previous_type == StorageLockType::kSetObjectTypeClass);

      v->variant_.emplace<ExclusiveLockedArgs>();
      // we already have a variant reference, do not acquire another
      return StorageReserveResult::kSuccess;
    }

    return StorageReserveResult::kDefiniteFailure;
  }

  if (lock_change.new_type == StorageLockType::kNone) {

    // drop current reference and release any type-specific locks

    if (lock_change.previous_type == StorageLockType::kSetObjectTypeClass) {
      // dropping type-specific locks should be an infallible operation, by
      // desigh
      [[maybe_unused]] auto res =
          perform_type_specific_lock_change(*v,
                                            lock_change.marginal_extra_lock);
      assert(res);
    }

    release_variant_reference(*v);
    return StorageReserveResult::kSuccess;
  }

  // new_type is StorageLockType::kSetObjectTypeClass.  prev_type is anything.

  if (lock_change.previous_type == StorageLockType::kNone) {
    // reconstruct caller's lock state
    std::optional<StorageLock> previous_lock = std::nullopt;

    assert(lock_change.new_typeclass);

    switch (lock_change.new_typeclass->type()) {
    case StorageObjectType::kBytestring:
      return try_reserve_bytestring_set(*v,
                                        lock_change.new_typeclass->bytestring(),
                                        previous_lock);
    case StorageObjectType::kNonnegativeInt64:
      return try_reserve_nnint64_set_add(
          *v, lock_change.new_typeclass->int64(),
          lock_change.marginal_extra_lock.nnint64_subtracted_amount(),
          previous_lock);
    default:
      throw std::runtime_error("unimplemented");
    }
  }

  if (lock_change.previous_type == StorageLockType::kExclusiveLock) {
    std::optional<StorageLock> previous_lock =
        StorageLock(StorageLockType::kExclusiveLock);

    assert(v->refcount_ == 1);
    assert(std::holds_alternative<ExclusiveLockedArgs>(v->variant_));

    // everything should be infallible, but using the try_reserve methods
    // to avoid code duplication.
    switch (lock_change.new_typeclass->type()) {
    case StorageObjectType::kBytestring:
      set_variant_from_exclusive_lock<BytestringArgs>(
          *v, lock_change.new_typeclass->bytestring());
      return try_reserve_bytestring_set(*v,
                                        lock_change.new_typeclass->bytestring(),
                                        previous_lock);
    case StorageObjectType::kNonnegativeInt64:
      set_variant_from_exclusive_lock<NNInt64SetAddArgs>(
          *v, lock_change.new_typeclass->int64());
      return try_reserve_nnint64_set_add(
          *v, lock_change.new_typeclass->int64(),
          lock_change.marginal_extra_lock.nnint64_subtracted_amount(),
          previous_lock);
    default:
      throw std::runtime_error("unimplemented");
    }
  }

  assert(lock_change.previous_type == StorageLockType::kSetObjectTypeClass);
  assert(v->refcount_ >= 1);
  assert(!std::holds_alternative<ExclusiveLockedArgs>(v->variant_));

  std::optional<StorageLock> previous_lock =
      StorageLock(StorageLockType::kExclusiveLock);

  switch (lock_change.marginal_extra_lock.object_type()) {
  case StorageObjectType::kNonnegativeInt64:
  {
    // not strictly true that the caller holds this many locked subtraction
    // units, However, 0 subtraction units are supplied to the function.
    previous_lock->extra_lock_data().nnint64_subtracted_amount() = 0;

    assert(std::holds_alternative<NNInt64SetAddArgs>(v->variant_));

    int64_t current_set_value =
        std::get<NNInt64SetAddArgs>(v->variant_).set_value_;

    int64_t new_subtract_amount =
        lock_change.marginal_extra_lock.nnint64_subtracted_amount();

    // if we need more locks, acquire more locks.
    if (new_subtract_amount <= 0) {
      return try_reserve_nnint64_set_add(
          *v, current_set_value,
          lock_change.marginal_extra_lock.nnint64_subtracted_amount(),
          previous_lock);
    }
    // else return the locks we have
    // We don't use try_reserve pathway here, as that would inadvertently mess
    // with the total_added_ variable instead of the total_subtracted_ variable

    std::get<NNInt64SetAddArgs>(v->variant_).total_subtracted_ +=
        new_subtract_amount;
    return StorageReserveResult::kSuccess;
  }
  default:
    throw std::runtime_error("this should be unreachable");
  }
}

StorageReserveResult
StorageObjectFrame::try_reserve_delta(StorageDelta const &delta,
                                      std::optional<StorageLock> &lock)
{
  auto v = variant_.take();

  if (lock.has_value()) {
    assert(lock.lock_type != StorageLockType::kNone);

    if constexpr (is_debug_build()) {
      if (lock->lock_type() == StorageLockType::kSetObjectTypeClass) {
        assert(!std::holds_alternative<ExclusiveLockedArgs>(v->variant_));
        assert(v->variant_.refcount_ != 0);
      }
      if (lock->lock_type() == StorageLockType::kExclusiveLock) {
        assert(std::holds_alternative<ExclusiveLockedArgs>(v->variant_));
        assert(v->variant_.refcount_ == 1);
      }
    }

    if (lock->lock_type() == StorageLockType::kExclusiveLock) {
      switch (delta.type()) {
      case StorageDeltaType::kDelete:
        set_variant_from_exclusive_lock<DeleteArgs>(*v);
        break;
      case StorageDeltaType::kBytestringSet:
        set_variant_from_exclusive_lock<BytestringArgs>(*v, delta.bytestring());
        break;
      case StorageDeltaType::kNonnegativeInt64SetAdd:
        set_variant_from_exclusive_lock<NNInt64SetAddArgs>(
            *v, delta.nnint64().set_value);
        break;
      default:
        throw std::runtime_error("unimpl");
      }
    }
  }

  switch (delta.type()) {
  case StorageDeltaType::kBytestringSet:
    return try_reserve_bytestring_set(*v, delta.bytestring(), lock);
  case StorageDeltaType::kNonnegativeInt64SetAdd:
    return try_reserve_nnint64_set_add(*v, delta.nnint64().set_value,
                                       delta.nnint64().add_value, lock);
  default:
    throw std::runtime_error("unknown delta type");
  }
}

/*
StorageReserveResult
StorageObjectFrame::try_reserve_delta(StorageDelta const &delta)
{
  switch (delta.type()) {
  case StorageDeltaType::kDeleteLast:
    return StorageReserveResult::kSuccess;
  case StorageDeltaType::kBytestringSet:
    return try_reserve_bytestring_set(delta.bytestring());
  case StorageDeltaType::kNonnegativeInt64SetAdd:
    return try_reserve_nnint64_set_add(delta.nnint64().set_value,
                                       delta.nnint64().add_value);
  case StorageDeltaType::kReplayCacheInsert:
    return try_reserve_replay_cache_inserts(delta.replay_nonces());
  case StorageDeltaType::kReplayCacheClear:
    return try_reserve_replay_cache_clear(delta.replay_clear_threshold());
  case StorageDeltaType::kMessageQueueAppend:
    return try_reserve_mq_append(delta.append_event());
  case StorageDeltaType::kMessageQueueConsume:
    return try_reserve_mq_consume(delta.num_messages_consumed());
  default:
    return StorageReserveResult::kDefiniteFailure;
  }
} */

void
StorageObjectFrame::commit_delta(StorageDelta const &delta)
{
  switch (delta.type()) {
  // case StorageDeltaType::kReplayCacheClear:
  //   commit_replay_cache_clear(delta.replay_clear_threshold());
  default:
    // no op
    return;
  }
}

void
StorageObjectFrame::revert_delta(StorageDelta const &delta)
{
  auto v = variant_.take();

  release_variant_reference(*v);

  switch (delta.type()) {
  case StorageDeltaType::kDelete:
    return;
  case StorageDeltaType::kBytestringSet:
    return;
  case StorageDeltaType::kNonnegativeInt64SetAdd:
    revert_nnint64_set_add(*v, delta.nnint64().add_value);
    return;
    /*  case StorageDeltaType::kReplayCacheInsert:
        revert_replay_cache_inserts(delta.replay_nonces());
        return;
      case StorageDeltaType::kReplayCacheClear:
        revert_replay_cache_clear();
        return;
      case StorageDeltaType::kMessageQueueAppend:
        revert_mq_append(delta.append_event());
        return;
      case StorageDeltaType::kMessageQueueConsume:
        revert_mq_consume();
        return; */
  }
}

std::optional<StorageObject> const &
StorageObjectFrame::get_committed_object() const
{
  return committed_base_object_;
}

std::optional<StrippedStorageObject>
StorageObjectFrame::get_stripped_committed_object() const
{
  if (!committed_base_object_) {
    return std::nullopt;
  }

  StrippedStorageObject obj(committed_base_object_->type());
  switch (obj.type()) {
  case StorageObjectType::kBytestring:
    obj.bytestring() = committed_base_object_->bytestring();
    break;
  case StorageObjectType::kNonnegativeInt64:
    obj.int64() = committed_base_object_->int64();
    break;
  case StorageObjectType::kBoundedHashset:
    obj.hashset() = committed_base_object_->hashset();
    break;
  case StorageObjectType::kReplayCache:
    obj.replay_cache_metadata() =
        committed_base_object_->replay_cache().metadata;
    break;
  case StorageObjectType::kMessageQueue:
    // TODO(geoff): something more efficient than copying the whole thing
    obj.message_queue() = committed_base_object_->message_queue();
    break;
  default:
    throw std::runtime_error("unimplemented");
  }
  return obj;
}

void
StorageObjectFrame::clear_modifications()
{
  variant_->refcount_ = 0;
}

void
StorageObjectFrame::commit_round()
{
  x23::Defer cleanup([&] { clear_modifications(); });

  auto v = variant_.take();

  if (v->refcount_ == 0) {
    // no committed modifications.
    return;
  }

  committed_base_object_ = std::visit(
      [&](auto const &arm) { return arm.to_object(committed_base_object_); },
      v->variant_);
}

void
StorageObjectFrame::revert_round()
{
  clear_modifications();
}

} // namespace hogd
