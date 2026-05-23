#include "hogd/hogdb/item_lock.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <type_traits>
#include <variant>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/balance_lock.h"
#include "hogd/hogdb/item_lock/bytestring_lock.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/deletion_lock.h"
#include "hogd/hogdb/item_lock/exclusive_lock.h"
#include "hogd/hogdb/item_lock/new_contract_lock.h"
#include "hogd/hogdb/item_lock/new_deployment_lock.h"
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/item_lock/replay_cache_lock.h"
#include "hogd/hogdb/status.h"
#include "retrocausal_queue.hh"
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
                                 const LockContext &ctx)
{
  auto ctx_with_snapshot = ctx;
  ctx_with_snapshot.item_lock_snapshot_cost = snapshot_cost_;

  auto lock_state = lock_state_.load(std::memory_order_acquire);

  if (lock_state == kFinalized) {
    return handle_try_atomic_from_finalized(before, after, ctx_with_snapshot);
  }

  if (lock_state == kFinalizedPermamentExclusiveLock) {
    return HogDBError{HogDBCode::kConflict};
  }
  CHECK_EQ(lock_state, kUncommitted);

  std::unique_lock<x23::TestLock> lock(lock_);

  if (before == nullptr) {
    CHECK_CONST_PTR(after);

    if constexpr (is_debug_build()) {
      if (ctx.key.is_address_balance()) {
        CHECK(after->is_balance());
      }
    }
    return handle_uncommitted_try_new_lock(after, ctx_with_snapshot, lock);
  }

  if (after == nullptr) {
    CHECK_CONST_PTR(before);
    handle_release_uncommitted_lock(before, ctx_with_snapshot, lock);
    // this value should be ignored, so returning a value that is obviously
    // invalid
    return std::numeric_limits<int128_t>::max();
  }

  CHECK_CONST_PTR(before);
  CHECK_CONST_PTR(after);
  CHECK_GT(pending_refcount_, 0U);
  // No need to modify pending_refcount_ here -- we already have a reference and
  // don't need to release it, as both before and after are non-null.
  return handle_try_atomic_from_pending(before, after, ctx_with_snapshot, lock);
}

int128_t
ItemLock::commit_change(const ItemChange &change, const LockContext &ctx)
{
  bool did_finalize = false;

  if (change.is_exclusive()) {
    std::unique_lock<x23::TestLock> lock(lock_);
    CHECK_EQ(pending_refcount_, 1U);

    prepare_lock_contents(&change, ctx, lock);

    std::visit(
        [&](auto &lock) -> void {
          using lock_t = std::decay_t<decltype(lock)>;
          if constexpr (std::is_same_v<lock_t, std::monostate> ||
                        std::is_same_v<lock_t, ExclusiveLock>) {
            panic("bad exclusive state");
          }
          else {
            using change_type = typename lock_t::change_type;
            auto const *inner = change.get_inner<change_type>();
            CHECK_CONST_PTR(inner);
            auto r = lock.try_atomic_change_lock(nullptr, inner, ctx);
            CHECK(r.has_value());
          }
        },
        lock_contents_);

    auto target_state = change.is_permanent_exclusive()
                            ? kFinalizedPermamentExclusiveLock
                            : kFinalized;
    auto old_state =
        lock_state_.exchange(target_state, std::memory_order_acq_rel);
    CHECK_EQ(old_state, kUncommitted);
    did_finalize = true;
  }
  else {
    auto old_state =
        lock_state_.exchange(kFinalized, std::memory_order_acq_rel);
    CHECK_NE(old_state, kFinalizedPermamentExclusiveLock);
    did_finalize = (old_state == kUncommitted);
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
          CHECK_CONST_PTR(inner);
          return lock.commit_change(did_finalize, snapshot_cost_, *inner, ctx);
        }
      },
      lock_contents_);
}

void
ItemLock::add_committed_balance_credit(int128_t delta)
{
  CHECK_EQ(lock_state_.load(std::memory_order_acquire), kFinalized);
  auto *inner = std::get_if<BalanceLock>(&lock_contents_);
  CHECK(inner != nullptr);
  inner->add_committed_credit(delta);
}

HogDBStatus<int128_t>
ItemLock::handle_try_atomic_from_finalized(const ItemChange *before,
                                           const ItemChange *after,
                                           const LockContext &ctx)
{
  CHECK(!std::holds_alternative<std::monostate>(lock_contents_));
  CHECK(!std::holds_alternative<ExclusiveLock>(lock_contents_));
  CHECK(before == nullptr || (!before->is_exclusive()));

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
            CHECK_CONST_PTR(before_inner);
          }

          change_type const *after_inner = nullptr;
          if (after != nullptr) {
            after_inner = after->get_inner<change_type>();
            if (after_inner == nullptr) {
              return HogDBError{HogDBCode::kConflict};
            }
          }
          return lock.try_atomic_change_lock(before_inner, after_inner, ctx);
        }
      },
      lock_contents_);
  if (!local_cost.has_value()) {
    return local_cost;
  }
  CHECK((before == nullptr || !before->is_permanent_exclusive()) &&
        (after == nullptr || !after->is_permanent_exclusive()));

  return snapshot_cost_transform(local_cost.value(), snapshot_cost_, false);
}

// pending_refcount_ is the number of transactions that have successfully
// acquired a ref on this lock.  Does not include current transaction if before
// is nullptr.
HogDBStatus<int128_t>
ItemLock::handle_try_atomic_from_pending(
    const ItemChange *before, const ItemChange *after, const LockContext &ctx,
    std::unique_lock<x23::TestLock> const &ul)
{
  CHECK(!std::holds_alternative<std::monostate>(lock_contents_));
  CHECK(ul.owns_lock());
  CHECK(before == nullptr || pending_refcount_ > 0);

  bool before_holds_exclusive_lock =
      (before != nullptr && before->is_exclusive());

  if (before_holds_exclusive_lock) {
    CHECK(std::holds_alternative<ExclusiveLock>(lock_contents_));
    // If after is nullptr, this case is handled elsewhere
    // (handle_release_uncommitted_lock)
    CHECK_CONST_PTR(after);

    if (after->is_exclusive()) {
      auto res = std::get<ExclusiveLock>(lock_contents_)
                     .try_atomic_change_lock(before, after, ctx);
      if (!res.has_value()) {
        return res;
      }
      return snapshot_cost_transform(*res, snapshot_cost_,
                                     after->is_permanent_exclusive());
    }

    // downgrade from exclusive to non-exclusive
    prepare_lock_contents(after, ctx, ul);

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
            CHECK_CONST_PTR(after_inner);
            return lock.try_atomic_change_lock(nullptr, after_inner, ctx);
          }
        },
        lock_contents_);
    if (!res.has_value()) {
      return res;
    }
    CHECK(!after->is_permanent_exclusive());
    return snapshot_cost_transform(res.value(), snapshot_cost_, false);
  }

  if (after != nullptr && after->is_exclusive()) {
    CHECK(!before_holds_exclusive_lock);
    // upgrading from non-exclusive to exclusive

    if (before == nullptr && pending_refcount_ > 0) {
      // we do not have a lock, and some other transaction already has a ref on
      // this lock. Cannot acquire in exclusive mode
      return HogDBError{HogDBCode::kConflict};
    }

    if (before != nullptr && pending_refcount_ > 1) {
      CHECK(!before->is_exclusive());
      // we have a lock, but some other transaction already has a ref on this
      // lock. Cannot acquire in exclusive mode
      return HogDBError{HogDBCode::kConflict};
    }

    CHECK((before == nullptr && pending_refcount_ == 0) ||
          (before != nullptr && pending_refcount_ == 1));

    set_exclusive_lock(ul);

    auto res = std::get<ExclusiveLock>(lock_contents_)
                   .try_atomic_change_lock(before, after, ctx);
    if (!res.has_value()) {
      return res;
    }
    return snapshot_cost_transform(*res, snapshot_cost_,
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
            CHECK_CONST_PTR(before_inner);
          }

          change_type const *after_inner = nullptr;
          if (after != nullptr) {
            after_inner = after->get_inner<change_type>();
            if (after_inner == nullptr) {
              return HogDBError{HogDBCode::kConflict};
            }
          }

          return lock.try_atomic_change_lock(before_inner, after_inner, ctx);
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
                                          const LockContext &ctx,
                                          std::unique_lock<x23::TestLock> &ul)
{
  if (pending_refcount_ == 0) {
    CHECK(std::holds_alternative<std::monostate>(lock_contents_));
    CHECK_CONST_PTR(after);
    prepare_lock_contents(after, ctx, ul);
  }

  auto res = handle_try_atomic_from_pending(nullptr, after, ctx, ul);

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
    const ItemChange *before, const LockContext &ctx,
    std::unique_lock<x23::TestLock> const &ul)
{
  CHECK(ul.owns_lock());
  CHECK_GT(pending_refcount_, 0U);
  CHECK_CONST_PTR(before);
  pending_refcount_--;
  if (pending_refcount_ == 0) {
    reset_lock_contents(ul);
    return;
  }

  auto res = handle_try_atomic_from_pending(before, nullptr, ctx, ul);
  if (!res.has_value()) {
    panic("failed to release uncommitted lock");
  }
}

void
ItemLock::set_exclusive_lock(std::unique_lock<x23::TestLock> const &ul)
{
  CHECK(ul.owns_lock());
  CHECK_LE(pending_refcount_, 1U);
  lock_contents_.emplace<ExclusiveLock>();
}

void
ItemLock::prepare_lock_contents(const ItemChange *after,
                                const LockContext & /*ctx*/,
                                std::unique_lock<x23::TestLock> const &ul)
{
  CHECK((pending_refcount_ == 0 &&
         std::holds_alternative<std::monostate>(lock_contents_)) ||
        (pending_refcount_ == 1 &&
         std::holds_alternative<ExclusiveLock>(lock_contents_)));

  CHECK_CONST_PTR(after);
  CHECK(ul.owns_lock());

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

  if (after->is_balance()) {
    CHECK(snapshot_type_ == kItemLockCacheDataBalance);
    lock_contents_.emplace<BalanceLock>(balance_base_);
    return;
  }

  if (after->is_deletion()) {
    lock_contents_.emplace<DeletionLock>();
    return;
  }

  if (after->is_create_contract()) {
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    lock_contents_.emplace<NewContractLock>(
        after->to_new_contract().value().new_contract());
    // NOLINTEND(bugprone-unchecked-optional-access)
    return;
  }

  if (after->is_deploy_contract()) {
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    lock_contents_.emplace<NewDeploymentLock>(
        after->to_new_deployment().value().deployed_contract_hash());
    // NOLINTEND(bugprone-unchecked-optional-access)
    return;
  }

  if (after->is_auth_data()) {
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    lock_contents_.emplace<PointerLock<AuthLock>>(
        after->to_auth_data()->base_auth_data());
    // NOLINTEND(bugprone-unchecked-optional-access)
    return;
  }

  if (after->is_replay_cache()) {
    // ReplayCacheLock lazily loads replay cache data in try_atomic_change_lock,
    // so we don't pass in base snapshot data here like we do with most types.
    lock_contents_.emplace<PointerLock<ReplayCacheLock>>();
    return;
  }

  if (after->is_retrocausal_queue()) {
    lock_contents_.emplace<PointerLock<RetrocausalQueueLock>>();
    return;
  }

  panic("unsupported change type");
}

void
ItemLock::reset_lock_contents(std::unique_lock<x23::TestLock> const &ul)
{
  CHECK(ul.owns_lock());
  CHECK_EQ(pending_refcount_, 0U);

  lock_contents_ = std::monostate{};
}

std::optional<QueueEntry>
ItemLock::get_concurrent_entry(uint32_t index) const
{
  auto lock_state = lock_state_.load(std::memory_order_acquire);
  if (lock_state == kUncommitted) {
    return std::nullopt;
  }

  return std::visit(
      [index](auto const &lock) -> std::optional<QueueEntry> {
        using lock_t = std::decay_t<decltype(lock)>;
        if constexpr (std::is_same_v<lock_t,
                                     PointerLock<RetrocausalQueueLock>>) {
          return lock.get()->get_concurrent_entry(index);
        }
        else {
          // Lock is in exclusive/deletion/other state (e.g. after a delete
          // that was followed by RCQ operations in the same tx). No concurrent
          // entries exist in this lock.
          return std::nullopt;
        }
      },
      lock_contents_);
}

HogDBStatus<void>
ItemLock::stage_committed_changes(const LockContext &ctx,
                                  ChangeSet &changes) const
{
  CHECK(is_finalized());
  CHECK(!std::holds_alternative<std::monostate>(lock_contents_));
  CHECK(!std::holds_alternative<ExclusiveLock>(lock_contents_));

  // Clean up auxiliary keys when the snapshot type had them but the committed
  // type doesn't. Currently only RCQ has auxiliary keys (block entries at
  // Prefix::kRetrocausalQueueBlock).
  //
  // Case 1 (snapshot RCQ → committed RCQ): handled by
  //   RetrocausalQueueLock::stage_committed_changes via normal consume pruning.
  // Case 2 (snapshot RCQ → committed non-RCQ, non-Deletion): handled here.
  // DeletionLock handles its own cleanup via DAO type read.
  if (snapshot_has_auxiliary_keys()) {
    // Currently only RCQ has auxiliary keys. If a new aux-key type is added
    // to snapshot_has_auxiliary_keys(), its cleanup path must be added here.
    CHECK(snapshot_type_ == kItemLockCacheDataRCQ);

    auto committed_is_rcq =
        std::holds_alternative<PointerLock<RetrocausalQueueLock>>(
            lock_contents_);
    auto committed_is_deletion =
        std::holds_alternative<DeletionLock>(lock_contents_);
    if (!committed_is_rcq && !committed_is_deletion) {
      if (auto r =
              RetrocausalQueueLock::cleanup_on_deletion(ctx, changes,
                                                        /*keep_metadata=*/true);
          !r) {
        return r;
      }
    }
  }

  return std::visit(
      [&](auto &lock) -> HogDBStatus<void> {
        using LockType = std::decay_t<decltype(lock)>;
        if constexpr (std::is_same_v<LockType, std::monostate> ||
                      std::is_same_v<LockType, ExclusiveLock>) {
          panic("bad finalized state");
        }
        else {
          return lock.stage_committed_changes(ctx, changes);
        }
      },
      lock_contents_);
}

} // namespace hogd
