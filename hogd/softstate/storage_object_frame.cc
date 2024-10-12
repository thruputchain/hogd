#include "hogd/softstate/storage_object_frame.h"

#include <atomic>
#include <cassert>
#include <optional>
#include <stdexcept>

#include "hogd/base/compiler.h"
#include "storage_object.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

namespace {

StorageObject
object_from_delta_class(StorageDeltaClass const &d,
                        std::optional<StorageObject> const &prev_object)
{
  StorageObject object;

  // Some types (hashsets) require copying over some data
  if (prev_object) {
    object = *prev_object;
    assert(object.type() == d.type());
  }
  else {
    object.type(d.type());
  }

  switch (d.type()) {
  case StorageObjectType::kBytestring:
    object.bytestring() = d.bytestring();
    break;
  case StorageObjectType::kNonnegativeInt64:
    object.int64() = d.nnint64_set_value();
  default:
    throw std::runtime_error("unimplemented");
  }

  return object;
}

StorageDeltaClass
delta_class_from_delta(StorageDelta const &delta)
{
  StorageDeltaClass out;

  switch (delta.type()) {
  case StorageDeltaType::kDeleteLast:
    throw std::runtime_error("this case is handled separately");
  case StorageDeltaType::kBytestringSet:
    out.type(StorageObjectType::kBytestring);
    out.bytestring() = delta.bytestring();
    break;
  default:
    throw std::runtime_error("unimplemented");
  }
  return out;
}

} // namespace

StorageReserveResult
StorageObjectFrame::try_reserve_delta(StorageDelta const &delta)
{
  if (delta.type() == StorageDeltaType::kDeleteLast) {
    return StorageReserveResult::kSuccess;
  }

  auto dcc_result = delta_class_checker_.try_set(delta_class_from_delta(delta));
  if (dcc_result != StorageReserveResult::kSuccess) {
    return dcc_result;
  }

  switch (delta.type()) {
  case StorageDeltaType::kDeleteLast:
    throw std::runtime_error("this case is handled separately");
  case StorageDeltaType::kBytestringSet:
    // nothing to do
    return StorageReserveResult::kSuccess;
  case StorageDeltaType::kNonnegativeInt64SetAdd:
  {
    if (delta.nnint64().add_value >= 0) {
      nnint64_total_added.add(delta.nnint64().add_value);
      return StorageReserveResult::kSuccess;
    }

    const int64_t set_value = delta.nnint64().set_value;
    if (set_value < 0) {
      return StorageReserveResult::kDefiniteFailure;
    }

    int64_t expect_sub =
        nnint64_total_subtracted.load(std::memory_order_relaxed);

    while (true) {
      int64_t desired_sub = 0;

      if (__builtin_add_overflow(expect_sub, delta.nnint64().add_value,
                                 &desired_sub)) {
        return StorageReserveResult::kDefiniteFailure;
      }

      // Cannot overflow.
      // set_value > 0 and desired_sub < 0.  So worst case
      // we get 1 + INT64_MIN, which does not overflow.
      if (set_value + desired_sub < 0) {
        return StorageReserveResult::kDefiniteFailure;
      }

      if (nnint64_total_subtracted.compare_exchange_strong(
              expect_sub, desired_sub, std::memory_order_relaxed)) {
        return StorageReserveResult::kSuccess;
      }

      cpu_relax();
    }
  }
  default:
    throw std::runtime_error("unimplemented");
  }
}

void
StorageObjectFrame::commit_delta(StorageDelta const &delta)
{
  if (delta.type() == StorageDeltaType::kDeleteLast) {
    deleted_last_.store(true, std::memory_order_release);
    return;
  }

  delta_class_checker_.commit_one();

  switch (delta.type()) {
  case StorageDeltaType::kDeleteLast:
    throw std::runtime_error("this case is handled separately");
  case StorageDeltaType::kBytestringSet:
    // nothing to do
    return;
  case StorageDeltaType::kNonnegativeInt64SetAdd:
    // nothing to do;
    return;
  default:
    throw std::runtime_error("unimplemented");
  }
}
void
StorageObjectFrame::revert_delta(StorageDelta const &delta)
{
  if (delta.type() == StorageDeltaType::kDeleteLast) {
    // nothing to do
    return;
  }
  delta_class_checker_.revert_one();

  switch (delta.type()) {
  case StorageDeltaType::kDeleteLast:
    throw std::runtime_error("this case is handled separately");
  case StorageDeltaType::kBytestringSet:
    // nothing to do
    return;
  case StorageDeltaType::kNonnegativeInt64SetAdd:
  {
    if (delta.nnint64().add_value < 0) {
      nnint64_total_subtracted.fetch_sub(delta.nnint64().add_value,
                                         std::memory_order_relaxed);
    }
    else {
      nnint64_total_added.sub(delta.nnint64().add_value);
    }
    break;
  }
  default:
    throw std::runtime_error("unimplemented");
  }
}

StorageObjectFrame::StorageObjectFrame(
    std::optional<StorageObject> const &snapshot_state)
  : committed_base_object_(snapshot_state),
    delta_class_checker_(snapshot_state), deleted_last_(false)
{}

std::optional<StorageObject> const &
StorageObjectFrame::get_committed_object() const
{
  return committed_base_object_;
}

void
StorageObjectFrame::clear_modifications()
{
  deleted_last_ = false;
  nnint64_total_subtracted = 0;
  nnint64_total_added.clear();
}

void
StorageObjectFrame::commit_round()
{
  // Create the new object, if necessary
  auto new_base_class = delta_class_checker_.commit_round();

  // populate the committed base object with any information from the delta
  // class
  if (new_base_class) {
    committed_base_object_ =
        object_from_delta_class(*new_base_class, committed_base_object_);
  }

  x23::Defer d([this] { clear_modifications(); });

  if (!committed_base_object_) {
    // If (as we should) this method is only called on objects that are touched
    // in a round, the only way we could get here is by deleting a key that does
    // not exist.
    return;
  }

  if (deleted_last_) {
    committed_base_object_ = std::nullopt;
    delta_class_checker_.clear_required_type();
    return;
  }

  if (committed_base_object_->type() == StorageObjectType::kNonnegativeInt64) {
    const int64_t nnint64_sub = nnint64_total_subtracted;
    const uint64_t nnint64_add = nnint64_total_added.get_capped();

    committed_base_object_->int64() += nnint64_sub;

    int64_t res;
    if (__builtin_add_overflow(committed_base_object_->int64(), nnint64_add,
                                 &res)) {
      committed_base_object_->int64() = INT64_MAX;
    }
  }
}

void
StorageObjectFrame::revert_round()
{
  delta_class_checker_.rewind_round();
  clear_modifications();
}

} // namespace hogd
