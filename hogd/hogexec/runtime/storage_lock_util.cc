#include <optional>

#include "hogd/base/compiler.h"
#include "hogd/hogexec/runtime/storage_lock_util.h"
#include "storage_object.hh"
#include "x23/format.h"

namespace hogd {

namespace {

StorageObjectType
object_type_from_delta_type(StorageDeltaType delta_type)
{
  switch (delta_type) {
  case StorageDeltaType::kDelete:
    return StorageObjectType::kDeleted;
  case StorageDeltaType::kBytestringSet:
    return StorageObjectType::kBytestring;
  case StorageDeltaType::kNonnegativeInt64SetAdd:
    return StorageObjectType::kNonnegativeInt64;
  case StorageDeltaType::kReplayCacheInsert:
  case StorageDeltaType::kReplayCacheClear:
    return StorageObjectType::kReplayCache;
  case StorageDeltaType::kMessageQueueAppend:
  case StorageDeltaType::kMessageQueueConsume:
    return StorageObjectType::kMessageQueue;
  default:
    throw std::runtime_error(
        "unknown delta type in object_type_from_delta_type");
  }
}

StorageObjectTypeClass
typeclass_from_delta(StorageDelta const &delta)
{
  StorageObjectTypeClass typeclass(object_type_from_delta_type(delta.type()));

  std::printf(std::format("delta: {}\n", x23::show(delta)).c_str());

  switch (typeclass.type()) {
  case StorageObjectType::kBytestring:
    typeclass.bytestring() = delta.bytestring();
    break;
  case StorageObjectType::kNonnegativeInt64:
    typeclass.int64() = delta.nnint64().set_value;
    break;
  default:
    // nothing to do
    break;
  }

  return typeclass;
}

constexpr FullStorageLock
make_exclusive_lock()
{
  return FullStorageLock(StorageLockType::kExclusiveLock);
}

} // namespace

FullStorageLock
upgrade_lock(std::optional<FullStorageLock> const &previous_lock,
             StorageDelta const &new_delta)
{
  StorageObjectTypeClass new_typeclass = typeclass_from_delta(new_delta);

  std::printf(std::format("starting lock upgrade: lock {} new_delta {}\n",
                          previous_lock.has_value() ? x23::show(*previous_lock)
                                                    : "nullopt",
                          x23::show(new_delta))
                  .c_str());

  if (!previous_lock.has_value()) {
    FullStorageLock new_lock(StorageLockType::kSetObjectTypeClass);
    auto &full_typeclass_lock = new_lock.full_typeclass_lock();
    full_typeclass_lock.typeclass = new_typeclass;

    full_typeclass_lock.extra_lock_data.object_type(new_typeclass.type());
    switch (new_typeclass.type()) {
    case StorageObjectType::kNonnegativeInt64:
      full_typeclass_lock.extra_lock_data.nnint64_subtracted_amount() =
          std::min<int64_t>(0, new_delta.nnint64().add_value);
      break;
    default:
      // nothing to do
      break;
    }
    return new_lock;
  }

  // previous_lock.has_value() == true

  if (previous_lock->lock_type() == StorageLockType::kSetObjectTypeClass) {
    auto &prev_typeclass_lock = previous_lock->full_typeclass_lock();

    if (prev_typeclass_lock.typeclass != new_typeclass) {
      // grab exclusive lock
      return make_exclusive_lock();
    }

    FullStorageLock out = *previous_lock;

    switch (new_typeclass.type()) {
    case StorageObjectType::kNonnegativeInt64:
    {
      int64_t max_subtraction = std::min<int64_t>(
          prev_typeclass_lock.extra_lock_data.nnint64_subtracted_amount(),
          new_delta.nnint64().add_value);
      out.full_typeclass_lock().extra_lock_data.nnint64_subtracted_amount() =
          max_subtraction;
      break;
    }
    default:
      // nothing to do
      break;
    }

    return out;
  }

  assert(previous_lock->lock_type() == StorageLockType::kExclusiveLock);

  return make_exclusive_lock();
}

StorageLock
strip_lock_data(FullStorageLock const& lock) {
  StorageLock out(lock.lock_type());
  switch (lock.lock_type()) {
  case StorageLockType::kSetObjectTypeClass:
    out.extra_lock_data() = lock.full_typeclass_lock().extra_lock_data;
    break;
  default:
    // nothing to do
    break;
  }

  return out;
}

namespace {

// transition from not kNone to kNone
StorageLockChange
release_all_locks(FullStorageLock const &lock)
{

  StorageLockChange out;
  out.previous_type = lock.lock_type();
  out.new_type = StorageLockType::kNone;

  if (lock.lock_type() == StorageLockType::kSetObjectTypeClass) {
    // don't need to set new_typeclass here
    out.marginal_extra_lock = lock.full_typeclass_lock().extra_lock_data;

    switch (out.marginal_extra_lock.object_type()) {
    case StorageObjectType::kNonnegativeInt64:
      // should never be able to acquire a lock to subtract INT64_MIN;
      // that would violate the nnint64>=0 invariant
      // Hence it is safe to negate this value.
      assert(out.marginal_extra_lock.nnint64_subtracted_amount() != INT64_MIN);

      out.marginal_extra_lock.nnint64_subtracted_amount() *= -1;
      break;
    default:
      // nothing to do;
      break;
    }
    return out;
  }
  assert(lock.lock_type() == StorageLockType::kExclusiveLock);
  return out;
}

// transition from not kExclusiveLock to kExclusiveLock
StorageLockChange
acquire_exclusive_lock(std::optional<FullStorageLock> const &previous_lock)
{
  StorageLockChange out;

  if constexpr (is_debug_build()) {
    if (previous_lock.has_value()) {
      assert(previous_lock->lock_type() != StorageLockType::kExclusiveLock);
    }
  }

  out.previous_type = (previous_lock.has_value() ? previous_lock->lock_type()
                                                 : StorageLockType::kNone);
  out.new_type = StorageLockType::kExclusiveLock;

  return out;
}

} // namespace

// can upgrade OR downgrade locks
std::optional<StorageLockChange>
compute_lock_change(std::optional<FullStorageLock> const &previous_lock,
                    std::optional<FullStorageLock> const &new_lock)
{
  if (!new_lock.has_value()) {
    if (!previous_lock.has_value()) {
      return std::nullopt;
    }

    std::printf("doing lock release\n");

    return release_all_locks(*previous_lock);
  }

  if (new_lock->lock_type() == StorageLockType::kExclusiveLock) {
    if (previous_lock.has_value() &&
        previous_lock->lock_type() == StorageLockType::kExclusiveLock) {
      return std::nullopt;
    }

    std::printf("doing exclusive acquire\n");

    return acquire_exclusive_lock(previous_lock);
  }

  assert(new_lock->lock_type() == StorageLockType::kSetObjectTypeClass);

  std::printf(std::format("new lock: {}\n", x23::show(*new_lock)).c_str());

  if ((!previous_lock.has_value()) ||
      (previous_lock->lock_type() == StorageLockType::kExclusiveLock)) {
    // (kNone or kExclusive) to kSetObjectTypeClass

    StorageLockChange out;
    out.previous_type =
        (previous_lock.has_value() ? StorageLockType::kExclusiveLock
                                   : StorageLockType::kNone);
    out.new_type = StorageLockType::kSetObjectTypeClass;

    out.new_typeclass.emplace(new_lock->full_typeclass_lock().typeclass);
    out.marginal_extra_lock = new_lock->full_typeclass_lock().extra_lock_data;

    return out;
  }

  assert(previous_lock->lock_type() == StorageLockType::kSetObjectTypeClass);
  assert(previous_lock->full_typeclass_lock().typeclass ==
         new_lock->full_typeclass_lock().typeclass);

  StorageLockChange out;

  out.previous_type = StorageLockType::kSetObjectTypeClass;
  out.new_type = StorageLockType::kSetObjectTypeClass;

  out.marginal_extra_lock.object_type(
      previous_lock->full_typeclass_lock().typeclass.type());

  switch (previous_lock->full_typeclass_lock().typeclass.type()) {
  case StorageObjectType::kNonnegativeInt64:
  {
    // compute marginal subtraction
    // both values nonpositive, not INT64_MIN, so subtraction is fine
    int64_t prev_sub_amount = previous_lock->full_typeclass_lock()
                                  .extra_lock_data.nnint64_subtracted_amount();
    int64_t new_sub_amount = new_lock->full_typeclass_lock()
                                 .extra_lock_data.nnint64_subtracted_amount();

    std::printf("new_sub_amount %lld prev_sub_amount %lld\n", new_sub_amount,
                prev_sub_amount);

    if (prev_sub_amount == new_sub_amount) {
      return std::nullopt;
    }
    assert(prev_sub_amount <= 0);
    assert(new_sub_amount <= 0);
    assert(new_sub_amount != INT64_MIN);

    out.marginal_extra_lock.nnint64_subtracted_amount() =
        (new_sub_amount - prev_sub_amount);
    return out;
  }
  default:
    return std::nullopt;
  }
  // unreachable
}

} // namespace hogd
