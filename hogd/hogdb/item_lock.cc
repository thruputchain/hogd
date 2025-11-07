#include "hogd/hogdb/item_lock.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <format>
#include <limits>
#include <mutex>
#include <type_traits>
#include <variant>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/bytestring_lock.h"
#include "hogd/hogdb/item_lock/deletion_lock.h"
#include "hogd/hogdb/item_lock/exclusive_lock.h"
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/testlock.h"

namespace hogd {

namespace {

int128_t
snapshot_cost_transform(int128_t local_cost, int128_t snapshot_cost,
                        bool is_permanent_exclusive)
{
  if (is_permanent_exclusive) {
    return local_cost - snapshot_cost;
  }
  return std::max<int128_t>(local_cost - snapshot_cost, 0);
}

} // namespace

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

  std::unique_lock<x23::TestLock> lock(lock_);

  if (before == nullptr) {
    assert(after != nullptr);
    return handle_uncommitted_try_new_lock(after, cost_model, lock);
  }

  if (after == nullptr) {
    assert(before != nullptr);
    handle_release_uncommitted_lock(before, cost_model, lock);
    // this value should be ignored, so returning a value that is obviously
    // invalid
    return std::numeric_limits<int128_t>::max();
  }

  assert(before != nullptr && after != nullptr);
  assert(pending_refcount_ > 0);
  // No need to modify pending_refcount_ here -- we already have a reference and
  // don't need to release it, as both before and after are non-null.
  return handle_try_atomic_from_pending(before, after, cost_model, lock);
}

int128_t
ItemLock::commit_change(const ItemChange &change,
                        const StorageCostModel *cost_model)
{
  bool did_finalize = false;

  auto lock_state = lock_state_.load(std::memory_order_acquire);
  // Release-acquire ensures that if lock_state_ != kUncommitted,
  // then the current thread will never read lock_change_ as ExclusiveLock.
  assert(lock_state != kFinalizedPermamentExclusiveLock);

  if (lock_state == kUncommitted) {
    // We only need to acquire the lock here if change_ is an exclusive change.
    // In that case, we wind up changing the variant of lock_contents_.

    if (change.is_exclusive()) {
      std::unique_lock<x23::TestLock> lock(lock_);
      assert(pending_refcount_ == 1);
      prepare_lock_contents(&change, lock);
    }

    // If change_ is not exclusive, then there may be
    // There may be a transaction that does not read that Finalized write,
    // and enters the try_atomic_change_lock state after reading kUncommitted
    // and acquires the lock there.  However, because we will not call
    // commit_change without first increasing pending_refcount_, any concurrent
    // calls to try_atomic_change_lock will not change the variant/reset the
    // variant of lock_contents_.

    if (change.is_permanent_exclusive()) {
      lock_state_.store(kFinalizedPermamentExclusiveLock,
                        std::memory_order_release);
    }
    else {
      lock_state_.store(kFinalized, std::memory_order_release);
    }
    did_finalize = true;
  }

  // Now we are in kFinalized state or kFinalizedPermamentExclusiveLock state.
  return std::visit(
      [&](auto &lock) -> int128_t {
        using lock_t = std::decay_t<decltype(lock)>;
        if constexpr (std::is_same_v<lock_t, std::monostate> ||
                      std::is_same_v<lock_t, ExclusiveLock>) {
          panic("bad finalized state");
        }
        else {
          auto const *inner = change.get_inner<typename lock_t::change_type>();
          assert(inner != nullptr);
          return lock.commit_change(did_finalize, snapshot_cost_, inner,
                                    cost_model);
        }
      },
      lock_contents_);
}

HogDBStatus<int128_t>
ItemLock::handle_try_atomic_from_finalized(const ItemChange *before,
                                           const ItemChange *after,
                                           const StorageCostModel *cost_model)
{
  assert(!std::holds_alternative<std::monostate>(lock_contents_));
  assert(!std::holds_alternative<ExclusiveLock>(lock_contents_));
  assert(before == nullptr || (!before->is_exclusive()));

  if (after != nullptr && after->is_exclusive()) {
    return HogDBError{HogDBCode::kConflict};
  }

  auto local_cost = std::visit(
      [&](auto &lock) -> HogDBStatus<int128_t> {
        using lock_t = std::decay_t<decltype(lock)>;

        if constexpr (std::is_same_v<lock_t, std::monostate> ||
                      std::is_same_v<lock_t, ExclusiveLock>) {
          panic("finalized bad state");
        }
        else {
          using change_type = typename lock_t::change_type;

          change_type const *before_inner = nullptr;
          if (before != nullptr) {
            before_inner = before->get_inner<change_type>();
            assert(before_inner != nullptr);
          }

          change_type const *after_inner = nullptr;
          if (after != nullptr) {
            after_inner = after->get_inner<change_type>();
            if (after_inner == nullptr) {
              return HogDBError{HogDBCode::kConflict};
            }
          }
          return lock.try_atomic_change_lock(before_inner, after_inner,
                                             cost_model);
        }
      },
      lock_contents_);
  if (!local_cost.has_value()) {
    return local_cost;
  }
  assert((before == nullptr || !before->is_permanent_exclusive()) &&
         (after == nullptr || !after->is_permanent_exclusive()));

  return snapshot_cost_transform(local_cost.value(), snapshot_cost_, false);
}

// pending_refcount_ is the number of transactions that have successfully
// acquired a ref on this lock.  Does not include current transaction if before
// is nullptr.
HogDBStatus<int128_t>
ItemLock::handle_try_atomic_from_pending(
    const ItemChange *before, const ItemChange *after,
    const StorageCostModel *cost_model,
    std::unique_lock<x23::TestLock> const &ul)
{
  assert(!std::holds_alternative<std::monostate>(lock_contents_));
  assert(ul.owns_lock());
  assert(before == nullptr || pending_refcount_ > 0);

  bool before_holds_exclusive_lock =
      (before != nullptr && before->is_exclusive());

  if (before_holds_exclusive_lock) {
    assert(std::holds_alternative<ExclusiveLock>(lock_contents_));
    // If after is nullptr, this case is handled elsewhere
    // (handle_release_uncommitted_lock)
    assert(after != nullptr);

    if (after->is_exclusive()) {
      auto res = std::get<ExclusiveLock>(lock_contents_)
                     .try_atomic_change_lock(before, after, cost_model);
      return snapshot_cost_transform(res, snapshot_cost_,
                                     after->is_permanent_exclusive());
    }

    // downgrade from exclusive to non-exclusive
    prepare_lock_contents(after, ul);

    // before might be a different type of change, which is incompatible with
    // the type-specific lock API.  So when downgrading from exclusive to
    // non-exclusive, we perform the type-specific locking as though no lock
    // were held.
    auto res = std::visit(
        [&](auto &lock) -> HogDBStatus<int128_t> {
          using lock_t = std::decay_t<decltype(lock)>;
          if constexpr (std::is_same_v<lock_t, std::monostate> ||
                        std::is_same_v<lock_t, ExclusiveLock>) {
            panic("bad pending state");
          }
          else {
            using change_type = typename lock_t::change_type;
            auto const *after_inner = after->get_inner<change_type>();
            assert(after_inner != nullptr);
            auto res =
                lock.try_atomic_change_lock(nullptr, after_inner, cost_model);
            assert(res.has_value());
            return res.value();
          }
        },
        lock_contents_);
    assert(res.has_value());
    return snapshot_cost_transform(res.value(), snapshot_cost_,
                                   after->is_permanent_exclusive());
  }

  if (after != nullptr && after->is_exclusive()) {
    assert(!before_holds_exclusive_lock);
    // upgrading from non-exclusive to exclusive

    if (before == nullptr && pending_refcount_ > 0) {
      // we do not have a lock, and some other transaction already has a ref on
      // this lock. Cannot acquire in exclusive mode
      return HogDBError{HogDBCode::kConflict};
    }

    if (before != nullptr && pending_refcount_ > 1) {
      assert(!before->is_exclusive());
      // we have a lock, but some other transaction already has a ref on this
      // lock. Cannot acquire in exclusive mode
      return HogDBError{HogDBCode::kConflict};
    }

    assert((before == nullptr && pending_refcount_ == 0) ||
           (before != nullptr && pending_refcount_ == 1));

    set_exclusive_lock(ul);

    auto res = std::get<ExclusiveLock>(lock_contents_)
                   .try_atomic_change_lock(before, after, cost_model);
    return snapshot_cost_transform(res, snapshot_cost_,
                                   after->is_permanent_exclusive());
  }

  auto local_cost = std::visit(
      [&](auto &lock) -> HogDBStatus<int128_t> {
        using lock_t = std::decay_t<decltype(lock)>;

        if constexpr (std::is_same_v<lock_t, std::monostate>) {
          panic("bad pendingstate");
        }
        else if constexpr (std::is_same_v<lock_t, ExclusiveLock>) {
          return HogDBError{HogDBCode::kConflict};
        }
        else {
          using change_type = typename lock_t::change_type;

          change_type const *before_inner = nullptr;
          if (before != nullptr) {
            before_inner = before->get_inner<change_type>();
            assert(before_inner != nullptr);
          }

          change_type const *after_inner = nullptr;
          if (after != nullptr) {
            after_inner = after->get_inner<change_type>();
            if (after_inner == nullptr) {
              return HogDBError{HogDBCode::kConflict};
            }
          }

          return lock.try_atomic_change_lock(before_inner, after_inner,
                                             cost_model);
        }
      },
      lock_contents_);

  if (!local_cost.has_value()) {
    return local_cost;
  }

  bool is_permanent_exclusive =
      after != nullptr && after->is_permanent_exclusive();
  return snapshot_cost_transform(local_cost.value(), snapshot_cost_,
                                 is_permanent_exclusive);
}

HogDBStatus<int128_t>
ItemLock::handle_uncommitted_try_new_lock(const ItemChange *after,
                                          const StorageCostModel *cost_model,
                                          std::unique_lock<x23::TestLock> &ul)
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
    std::unique_lock<x23::TestLock> const &ul)
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
ItemLock::set_exclusive_lock(std::unique_lock<x23::TestLock> const &ul)
{
  assert(ul.owns_lock());
  assert(pending_refcount_ <= 1);
  lock_contents_.emplace<ExclusiveLock>();
}

void
ItemLock::prepare_lock_contents(const ItemChange *after,
                                std::unique_lock<x23::TestLock> const &ul)
{
  assert((pending_refcount_ == 0 &&
          std::holds_alternative<std::monostate>(lock_contents_)) ||
         (pending_refcount_ == 1 &&
          std::holds_alternative<ExclusiveLock>(lock_contents_)));

  assert(after != nullptr);
  assert(ul.owns_lock());

  if (after->is_bytes()) {
    lock_contents_.emplace<BytestringLock>(after->to_bytes());
    return;
  }

  if (after->is_nnint128()) {
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    lock_contents_.emplace<NNInt128Lock>(
        after->get_raw_nnint128().value().base());
    // NOLINTEND(bugprone-unchecked-optional-access)
    return;
  }

  if (after->is_deletion()) {
    lock_contents_.emplace<DeletionLock>();
    return;
  }

  panic("unsupported change type");
}

void
ItemLock::reset_lock_contents(std::unique_lock<x23::TestLock> const &ul)
{
  assert(ul.owns_lock());
  assert(pending_refcount_ == 0);

  lock_contents_ = std::monostate{};
}

HogDBStatus<void>
ItemLock::write_committed_change(ItemKey const &key, HogDBDAO &dao,
                                 MerkleTriePrefix &trie) const
{
  assert(is_finalized());
  assert(!std::holds_alternative<std::monostate>(lock_contents_));
  assert(!std::holds_alternative<ExclusiveLock>(lock_contents_));
  return std::visit(
      [&](auto &lock) -> HogDBStatus<void> {
        if constexpr (std::is_same_v<std::decay_t<decltype(lock)>,
                                     std::monostate> ||
                      std::is_same_v<std::decay_t<decltype(lock)>,
                                     ExclusiveLock>) {
          panic("bad finalized state");
        }
        else {
          return lock.write_committed_change(key, dao, trie);
        }
      },
      lock_contents_);
}

} // namespace hogd
