#include "hogd/softstate/storage_object_frame.h"

#include <atomic>
#include <cassert>
#include <optional>
#include <stdexcept>

#include "x23/cleanup.h"

#include "storage_object.hh"
#include "types.hh"

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
  } else {
    object.type(d.type());
  }

  switch (d.type()) {
  case StorageObjectType::kBytestring:
    object.bytestring() = d.bytestring();
    break;
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
}

void
StorageObjectFrame::revert_round()
{
  delta_class_checker_.rewind_round();
  clear_modifications();
}

} // namespace hogd
