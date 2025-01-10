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
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

StorageItemFrame::StorageItemFrame(
    std::optional<StorageItem> const &snapshot_state)
  : committed_base_item_(snapshot_state)
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
  if (auto *args = try_set_variant<NNIntSetAddArgs>(set_value)) {
    if (add_value >= 0) {
      args->total_added_ += add_value;
      acquire_reference();
      return true;
    }

    // if set_value <=0 and add_value < 0, delta always fails
    if (set_value <= 0) {
      return false;
    }

    // set_value > 0, add_value < 0

    int128_t desired_subtracted = 0;

    if (integer_add_overflow(args->total_subtracted_, add_value,
                             &desired_subtracted)) {
      // more than INT128_MIN is subtracted from the counter.
      // This is impossible, if we want to maintain that set+add >= 0.

      return false;
    }

    // desired_subtracted < 0, set_value > 0 => no overflow on addition
    if (desired_subtracted + set_value < 0) {
      // this new subtraction would drive the value below 0
      return false;
    }

    args->total_subtracted_ = desired_subtracted;
    acquire_reference();
    return true;
  }
  return false;
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
StorageItemFrame::try_reserve_delta(StorageDelta const &delta)
{
  auto v = variant_.take();

  switch (delta.type()) {
  case StorageDeltaType::kBytestringSet:
    return v->try_reserve_bytestring_set(delta.bytestring());
  case StorageDeltaType::kNonnegativeIntSetAdd:
    return v->try_reserve_nnint_set_add(from_xdr(delta.nnint().set_value),
                                        from_xdr(delta.nnint().add_value));
  case StorageDeltaType::kDelete:
    return v->try_reserve_delete();
  default:
    throw std::runtime_error("unknown delta type");
  }
}

void
StorageItemFrame::commit_delta(StorageDelta const &delta)
{
  // no op
}

void
StorageItemFrame::revert_delta(StorageDelta const &delta)
{
  auto v = variant_.take();
  v->release_reference();

  switch (delta.type()) {
  case StorageDeltaType::kDelete:
  case StorageDeltaType::kBytestringSet:
    // nothing to do in these cases, beyond releasing the typeclass reference
    // above
    return;
  case StorageDeltaType::kNonnegativeIntSetAdd:
    v->revert_nnint_set_add(from_xdr(delta.nnint().add_value));
    return;
  default:
    throw std::runtime_error("unimplemented");
  }
}

std::optional<StorageItem> const &
StorageItemFrame::get_snapshot_item() const
{
  return committed_base_item_;
}

void
StorageItemFrame::clear_modifications()
{
  variant_->refcount_ = 0;
}

void
StorageItemFrame::commit_round()
{
  x23::Defer cleanup([&] { clear_modifications(); });

  auto v = variant_.take();

  if (v->refcount_ == 0) {
    // no committed modifications.
    return;
  }

  committed_base_item_ =
      std::visit([&](auto const &arm) { return arm.to_item(); }, v->variant_);
}

void
StorageItemFrame::revert_round()
{
  clear_modifications();
}

} // namespace hogd
