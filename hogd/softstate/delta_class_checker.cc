#include "hogd/softstate/delta_class_checker.h"

#include <atomic>
#include <cassert>
#include <cstdint>

#include "storage_object.hh"

namespace hogd {
/*
Tag format:
[active_open_tx_count 31b][finalized 1b]
*/
bool
is_finalized(uint32_t tag)
{
  return tag & 0x1;
}

void
bump_active_thread_count(uint32_t &tag)
{
  tag += 2;
}

void
drop_active_thread_count(uint32_t &tag)
{
  tag -= 2;
}

void
finalize(uint32_t &tag)
{
  tag |= 1;
}

StorageReserveResult
StorageDeltaClassChecker::try_set(StorageDeltaClass const &new_obj)
{
  if (required_type_) {
    if (new_obj.type() != *required_type_) {
      return StorageReserveResult::kDefiniteFailure;
    }
  }

  const auto *finalized =
      finalized_delta_class_.load(std::memory_order_relaxed);

  if (finalized) {
    return (*finalized == new_obj) ? StorageReserveResult::kSuccess
                                   : StorageReserveResult::kDefiniteFailure;
  }

  std::lock_guard lock(mtx_);

  if (is_finalized(tag_)) {
    assert(!!sdc_);
    return (*sdc_ == new_obj) ? StorageReserveResult::kSuccess
                              : StorageReserveResult::kDefiniteFailure;
  }

  if (sdc_ == nullptr) {
    sdc_ = std::make_unique<StorageDeltaClass>(new_obj);
    bump_active_thread_count(tag_);
    return StorageReserveResult::kSuccess;
  }

  if (*sdc_ == new_obj) {
    bump_active_thread_count(tag_);
    return StorageReserveResult::kSuccess;
  }

  return StorageReserveResult::kMaybeConflict;
}

void
StorageDeltaClassChecker::revert_one()
{
  if (finalized_delta_class_.load(std::memory_order_relaxed) != nullptr) {
    return;
  }

  std::lock_guard lock(mtx_);

  if (is_finalized(tag_)) {
    return;
  }

  drop_active_thread_count(tag_);

  if (tag_ == 0) {
    sdc_.reset();
  }
}

void
StorageDeltaClassChecker::commit_one()
{
  if (finalized_delta_class_.load(std::memory_order_relaxed) != nullptr) {
    return;
  }

  std::lock_guard lock(mtx_);
  finalize(tag_);
  finalized_delta_class_.store(sdc_.get(), std::memory_order_relaxed);
}

StorageDeltaClassChecker::StorageDeltaClassChecker(
    std::optional<StorageObject> const &base_object)
  : required_type_(base_object.has_value()
                       ? std::optional<StorageObjectType>(base_object->type())
                       : std::nullopt)
{}

std::optional<StorageDeltaClass>
StorageDeltaClassChecker::commit_round()
{
  tag_ = 0;
  if (!sdc_) {
    return std::nullopt;
  }

  required_type_ = sdc_->type();

  StorageDeltaClass out = *sdc_;

  sdc_.reset();

  return out;
}

void
StorageDeltaClassChecker::rewind_round()
{
  tag_ = 0;
  sdc_.reset();
}

void
StorageDeltaClassChecker::clear_required_type()
{
  required_type_ = std::nullopt;
}

} // namespace hogd
