#include "hogd/hogdb/item_lock.h"

#include <atomic>
#include <cassert>
#include <mutex>
#include <type_traits>
#include <variant>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/bytestring_lock.h"
#include "hogd/hogdb/item_lock/exclusive_lock.h"
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

HogDBStatus<int128_t>
ItemLock::try_atomic_change_lock(const ItemChange *before,
                                 const ItemChange *after,
                                 const StorageCostModel *cost_model)
{
  auto lock_state = lock_state_.load(std::memory_order_relaxed);

  if (lock_state == kFinalized) {
    return handle_try_atomic_from_finalized(before, after, cost_model);
  }

  if (lock_state == kFinalizedPermamentExclusiveLock) {
    return HogDBError{HogDBCode::kConflict};
  }
  assert(lock_state == kUncommitted);

  std::unique_lock lock(mtx_);

  if (before == nullptr) {
    assert(after != nullptr);
    return handle_uncommitted_try_new_lock(after, cost_model, lock);
  }

  if (after == nullptr) {
    assert(before != nullptr);
    handle_release_uncommitted_lock(before, cost_model, lock);
  }

  assert(before != nullptr && after != nullptr);
  assert(pending_refcount_ > 0);
  // No need to modify pending_refcount_ here -- we already have a reference and
  // don't need to release it, as both before and after are non-null.
  return handle_try_atomic_from_pending(before, after, cost_model, lock);
}

HogDBStatus<int128_t>
ItemLock::handle_try_atomic_from_finalized(const ItemChange *before,
                                           const ItemChange *after,
                                           const StorageCostModel *cost_model)
{
  assert(!std::holds_alternative<std::monostate>(lock_contents_));
  assert(!std::holds_alternative<ExclusiveLock>(lock_contents_));

  return std::visit(
      [&](auto &lock) -> HogDBStatus<int128_t> {
        using lock_t = std::decay_t<decltype(lock)>;

        if constexpr (std::is_same_v<lock_t, std::monostate> ||
                      std::is_same_v<lock_t, ExclusiveLock>) {
          panic("finalized bad state");
        }
        else {

          using change_type = typename lock_t::change_type;

          auto const *before_inner =
              (before != nullptr) ? before->get_inner<change_type>() : nullptr;
          auto const *after_inner =
              (after != nullptr) ? after->get_inner<change_type>() : nullptr;

          return lock.try_atomic_change_lock(before_inner, after_inner,
                                             cost_model);
        }
      },
      lock_contents_);
}

HogDBStatus<int128_t>
ItemLock::handle_try_atomic_from_pending(const ItemChange *before,
                                         const ItemChange *after,
                                         const StorageCostModel *cost_model,
                                         std::unique_lock<std::mutex> const &ul)
{
  assert(!std::holds_alternative<std::monostate>(lock_contents_));
  assert(ul.owns_lock());

  return std::visit(
      [&](auto &lock) -> HogDBStatus<int128_t> {
        using lock_t = std::decay_t<decltype(lock)>;

        if constexpr (std::is_same_v<lock_t, std::monostate>) {
          panic("bad pendingstate");
        }
        else if constexpr (std::is_same_v<lock_t, ExclusiveLock>) {
          return lock.try_atomic_change_lock(before, after, cost_model);
        }
        else {

          using change_type = typename lock_t::change_type;

          auto const *before_inner =
              (before != nullptr) ? before->get_inner<change_type>() : nullptr;
          auto const *after_inner =
              (after != nullptr) ? after->get_inner<change_type>() : nullptr;

          return lock.try_atomic_change_lock(before_inner, after_inner,
                                             cost_model);
        }
      },
      lock_contents_);
}

HogDBStatus<int128_t>
ItemLock::handle_uncommitted_try_new_lock(const ItemChange *after,
                                          const StorageCostModel *cost_model,
                                          std::unique_lock<std::mutex> &ul)
{
  if (pending_refcount_ == 0) {
    assert(std::holds_alternative<std::monostate>(lock_contents_));
    assert(after != nullptr);
    prepare_lock_contents(after, ul);
  }

  auto res = handle_try_atomic_from_pending(nullptr, after, cost_model, ul);

  if (!res.has_value()) {
    // failed to acquire lock
    if (pending_refcount_ == 0) {
      reset_lock_contents(ul);
    }
    return res;
  }

  pending_refcount_++;
  return res;
}

void
ItemLock::handle_release_uncommitted_lock(
    const ItemChange *before, const StorageCostModel *cost_model,
    std::unique_lock<std::mutex> const &ul)
{
  assert(ul.owns_lock());
  assert(pending_refcount_ > 0);
  assert(before != nullptr);
  pending_refcount_--;
  if (pending_refcount_ == 0) {
    reset_lock_contents(ul);
    return;
  }

  auto res = handle_try_atomic_from_pending(before, nullptr, cost_model, ul);
  if (!res.has_value()) {
    panic("failed to release uncommitted lock");
  }
}

void
ItemLock::prepare_lock_contents(const ItemChange *after,
                                std::unique_lock<std::mutex> const &ul)
{
  assert((pending_refcount_ == 0 &&
          std::holds_alternative<std::monostate>(lock_contents_)) ||
         (pending_refcount_ == 1 &&
          std::holds_alternative<ExclusiveLock>(lock_contents_)));

  assert(after != nullptr);
  assert(ul.owns_lock());

  if (after->is_bytes()) {
    lock_contents_.emplace<BytestringLock>(after->to_bytes());
  }
  else if (after->is_nnint128()) {
    lock_contents_.emplace<NNInt128Lock>(
        after->get_raw_nnint128().value().base());
  }
  else {
    panic("unsupported change type");
  }
}

void
ItemLock::reset_lock_contents(std::unique_lock<std::mutex> const &ul)
{
  assert(ul.owns_lock());
  assert(pending_refcount_ == 0);

  lock_contents_ = std::monostate{};
}

} // namespace hogd
