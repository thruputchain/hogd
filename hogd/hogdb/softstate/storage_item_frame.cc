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
    std::optional<StorageItem> const &snapshot_state)
  : addr_key_(addrkey), committed_item_(snapshot_state)
{}

std::optional<StorageItem>
StorageItemFrame::BytestringArgs::to_item() const
{
  StorageItem item(StorageItemType::kBytestring);
  item.bytestring() = bytestring_set_;
  return item;
}

std::optional<StorageItem>
StorageItemFrame::NNIntSetAddArgs::to_item() const
{
  StorageItem item;
  item.type(StorageItemType::kNonnegativeInt);

  int128_t new_value = set_value_ + total_subtracted_;
  int128_t total_add = total_added_.get_capped();

  if (integer_add_overflow<int128_t>(new_value, total_add)) {
    item.nnint() = to_xdr(std::numeric_limits<int128_t>::max());
  }
  else {
    item.nnint() = to_xdr(new_value + total_add);
  }

  return item;
}

template<typename VariantArm, typename... VariantArgs>
VariantArm *
StorageItemFrame::RefcountedVariant::try_set_variant(VariantArgs const &...args)
{
  if (refcount_ == 0) {
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
StorageItemFrame::RefcountedVariant::try_reserve_nnint_set_add(
    int128_t set_value, int128_t add_value)
{
  if (try_set_variant<NNIntSetAddArgs>(set_value)) {
    if (add_value >= 0) {
      // modifying total_added_ is deferred to commit time
      acquire_reference();
      return true;
    }

    // if set_value <=0 and add_value < 0, delta always fails
    if (set_value <= 0) {
      return false;
    }

    // the interesting case;
    // set_value > 0, add_value < 0
    auto res = try_reserve_nnint_additional(add_value);
    if (!res) {
      return false;
    }

    acquire_reference();
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
  assert(refcount_ > 0);

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
  auto &args = get_variant_arm_unchecked<NNIntSetAddArgs>();

  if (add_value >= 0) {
    args.total_added_ -= add_value;
  }
  else {
    args.total_subtracted_ -= add_value;
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
          return typeclass.item_type() == StorageItemType::kDeleted;
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
  switch (delta.type()) {
  case StorageDeltaType::kBytestringSet:
    return variant_.try_reserve_bytestring_set(delta.bytestring());
  case StorageDeltaType::kNonnegativeIntSetAdd:
    return variant_.try_reserve_nnint_set_add(from_xdr(delta.nnint().set_value),
                                              from_xdr(
                                                  delta.nnint().add_value));
  case StorageDeltaType::kDelete:
    return variant_.try_reserve_delete();
  default:
    throw std::runtime_error("unknown delta type");
  }
}

void
StorageItemFrame::commit_delta(StorageDelta const &delta)
{
  switch(delta.type()) {
  case StorageDeltaType::kBytestringSet:
    break;
  case StorageDeltaType::kNonnegativeIntSetAdd:
    variant_.commit_nnint_add(from_xdr(delta.nnint().add_value));
    break;
  case StorageDeltaType::kDelete:
    break;
  }
}

std::optional<StorageItem> const &
StorageItemFrame::get_snapshot_item() const
{
  assert(!round_committed_);
  return committed_item_;
}

std::optional<StorageItem> const &
StorageItemFrame::get_post_close_block_item()
{
  update_committed_item();
  return committed_item_;
}

void
StorageItemFrame::update_committed_item()
{
  if (!round_committed_) {
    if (variant_.refcount_ > 0) {
      committed_item_ =
          std::visit([&](auto const &arm) { return arm.to_item(); },
                     variant_.variant_);
    }
    round_committed_ = true;
  }
}

bool
StorageItemFrame::try_acquire_exclusive_lock(StorageLockType previous_lock_type)
{
  if (variant_.refcount_ == 0) {
    assert(previous_lock_type == StorageLockType::kNone);
    variant_.variant_ = ExclusiveLockedState{};
    variant_.acquire_reference();
    return true;
  }

  if (previous_lock_type == StorageLockType::kSetItemTypeClass) {
    // no need to acquire a new lock, we already have one
    return variant_.refcount_ == 1;
  }
  assert(previous_lock_type != StorageLockType::kExclusiveLock);
  return false;
}

void
StorageItemFrame::revert_exclusive_lock_to_none()
{
  variant_.release_reference();
  variant_.variant_ = DeleteArgs{};
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
  variant_.release_reference();

  switch (type_specific_lock.item_type()) {
  case StorageItemType::kNonnegativeInt:
    variant_.revert_nnint_set_add(
        from_xdr(type_specific_lock.nnint_subtracted_amount()));
    return;
  default:
    return;
  }
}

namespace {

StorageItemTypeClass
to_typeclass(StorageDelta const &delta)
{
  switch (delta.type()) {
  case StorageDeltaType::kBytestringSet:
    return StorageItemTypeClass(StorageItemType::kBytestring);
  case StorageDeltaType::kNonnegativeIntSetAdd:
  {
    auto out = StorageItemTypeClass(StorageItemType::kNonnegativeInt);
    out.nnint_set() = delta.nnint().set_value;
    return out;
  }
  case StorageDeltaType::kDelete:
    return StorageItemTypeClass(StorageItemType::kDeleted);
  }
  throw std::runtime_error("impossible");
}

StorageLock
to_lock(StorageDelta const &delta)
{
  StorageLock lock(StorageLockType::kSetItemTypeClass);
  lock.full_typeclass_lock().typeclass = to_typeclass(delta);
  switch (delta.type()) {
  case StorageDeltaType::kNonnegativeIntSetAdd:
    lock.full_typeclass_lock().extra_lock_data.item_type(
        StorageItemType::kNonnegativeInt);
    lock.full_typeclass_lock().extra_lock_data.nnint_subtracted_amount() =
        to_xdr(std::min<int128_t>(0, from_xdr(delta.nnint().add_value)));
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
  switch (previous_lock.lock_type()) {
  case StorageLockType::kNone:
  {
    if (try_reserve_delta(delta)) {
      return to_lock(delta);
    }
    return std::nullopt;
  }
  case StorageLockType::kSetItemTypeClass:
  {
    auto req_lock = to_lock(delta);
    assert(req_lock.type() == StorageLockType::kSetItemTypeClass);

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

    if (req_lock.full_typeclass_lock().extra_lock_data.item_type() !=
        StorageItemType::kNonnegativeInt) {
      // nothing to do here, we already have the lock
      // (and already have a reference included in refcount_)
      return previous_lock;
    }

    // item_type == kNonnegativeInt

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

    auto reserve_res =
        variant_.try_reserve_nnint_additional(req_sub_amount - prev_sub_amount);
    if (!reserve_res) {
      return std::nullopt;
    }

    return req_lock;
  }
  // always succeeds
  case StorageLockType::kExclusiveLock:
    return previous_lock;
  default:
    throw std::runtime_error("unimplemented");
  }
}

void
StorageItemFrame::revert_set_typeclass_lock_to_set_typeclass(
    StorageLock const &lock, StorageDelta const &delta)
{
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
    assert(locked_sub_amount - actual_sub_amount < 0);

    variant_.revert_nnint_set_add(locked_sub_amount - actual_sub_amount);
    break;
  }
  case StorageDeltaType::kDelete:
    // nothing to do
    break;
  }
}

void
StorageItemFrame::commit(StorageDelta const &delta, StorageLock const &lock)
{
  assert(lock.lock_type() != StorageLockType::kNone);
  switch (lock.lock_type()) {
  case StorageLockType::kSetItemTypeClass:
    revert_set_typeclass_lock_to_set_typeclass(lock, delta);
    commit_delta(delta);
    return;
  case StorageLockType::kExclusiveLock:
    revert_exclusive_lock_to_set_typeclass(delta);
    commit_delta(delta);
    return;
  default:
    throw std::runtime_error("impossible");
  }
}

void
StorageItemFrame::revert(StorageLock const &lock)
{
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
    revert_exclusive_lock_to_none();
    break;
  }

  assert(variant_.refcount_ == initial_refcount - 1);
}

} // namespace hogd
