#include "hogd/hogdb/block_tx.h"

#include <cassert>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_rpc.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/lthash16.h"
#include "hogd/hogdb/db_hash.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock.h"
#include "hogd/hogdb/item_lock/auth_lock.h"
#include "hogd/hogdb/item_lock/balance_lock.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/deletion_lock.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/status.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<int128_t>
BlockTX::do_lock(ItemLock &lock, const ItemChange &change,
                 const LockContext &ctx)
{
  return lock.try_atomic_change_lock(nullptr, &change, ctx);
}

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<int128_t>
BlockTX::do_modify_lock(ItemLock &lock, const ItemChange &old_change,
                        const ItemChange &new_change, const LockContext &ctx)
{
  return lock.try_atomic_change_lock(&old_change, &new_change, ctx);
}

ATTRIBUTE_ALWAYS_INLINE void
BlockTX::do_unlock_item(ItemLock &lock, const ItemChange &change,
                        const LockContext &ctx)
{
  auto r = lock.try_atomic_change_lock(&change, nullptr, ctx);
  CHECK(r.has_value());
}

void
BlockTX::release_balance(const Address &addr, int128_t delta)
{
  CHECK_GT(delta, 0);

  ItemKey key = make_address_balance_item_key(addr);
  auto *lock = addr_balance_locks_.insert_or_get(addr, [&] {
    return get_snapshot_cost_and_cache_data(key);
  });
  CHECK_PTR(lock);

  if (lock->is_finalized()) {
    lock->add_committed_balance_credit(delta);
  }
  else {
    // No prior balance lock exists in this block. This happens when
    // lock_storage was never called for the address (marginal cost was 0)
    // but commit_change returned a negative cost (e.g., consuming or
    // deleting snapshot entries). Such an address must have a snapshot
    // balance — you can't have snapshot storage items without one — so
    // the fresh-balance guard in try_atomic_change_lock passes.
    ItemChange change = ItemChange(Balance(delta));
    LockContext ctx(&db_, block_height_, key, &cost_model_);
    auto r = do_lock(*lock, change, ctx);
    CHECK(r.has_value());
    auto cr = lock->commit_change(std::move(change), ctx);
    CHECK(cr.has_value());
  }
}

HogDBStatus<std::pair<int128_t, ItemLockCacheData>>
BlockTX::get_snapshot_cost_and_cache_data(const ItemKey &key)
{
  auto snapshot_cost =
      db_.get_charged_storage_cost_and_cache_data(key, &cost_model_);
  if (!snapshot_cost) {
    if (snapshot_cost.error() != HogDBCode::kNotFound) {
      return snapshot_cost.error();
    }
    assert(snapshot_cost.error() == HogDBCode::kNotFound);
    assert(!key.is_address_balance());

    return std::make_pair(0, ItemLockCacheData::from_none());
  }
  return *snapshot_cost;
}

HogDBStatus<int128_t>
BlockTX::lock(const ItemKey &key, ItemChange &change)
{
  if (auto s = check_stale();
      !s || mode_ == BlockExecutionMode::kBlockExecutionModeViewing)
      [[unlikely]]
    return s ? HogDBError(HogDBCode::kInvalidExecutorBehavior) : s.error();
  // no storage cost for address balances.
  if (key.is_address_balance()) {

    auto *lock = addr_balance_locks_.insert_or_get(key.get_address(), [&] {
      return get_snapshot_cost_and_cache_data(
          make_address_balance_item_key(key.get_address()));
    });

    LockContext ctx(&db_, block_height_, key, &cost_model_);
    return do_lock(*lock, change, ctx)
        .and_then([](auto /*unused*/) -> HogDBStatus<int128_t> { return 0; });
  }

  auto *item = item_locks_.insert_or_get(key, [&] {
    return get_snapshot_cost_and_cache_data(key);
  });

  assert(item != nullptr);

  LockContext ctx(&db_, block_height_, key, &cost_model_);
  return do_lock(*item, change, ctx);
}

HogDBStatus<int128_t>
BlockTX::modify_lock(const ItemKey &key, const ItemChange &old_change,
                     const ItemChange &new_change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {
    auto *lock = addr_balance_locks_.get(key.get_address());
    CHECK_PTR(lock);
    LockContext ctx(&db_, block_height_, key, &cost_model_);
    return do_modify_lock(*lock, old_change, new_change, ctx)
        .and_then([](auto /*unused*/) -> HogDBStatus<int128_t> { return 0; });
  }

  auto *lock = item_locks_.get(key);
  CHECK_PTR(lock);

  LockContext ctx(&db_, block_height_, key, &cost_model_);
  auto r = do_modify_lock(*lock, old_change, new_change, ctx);
  if (!r && is_invalidated()) [[unlikely]] {
    // Catches the narrow race where a sibling invalidated us *during*
    // do_modify_lock: its DAO block removal made an RCQ consume's past-block
    // re-read fail (e.g. kNotFound). This re-check is reliable because RAMDB
    // serializes that removal and this read through the same per-partition
    // mutex, so observing the removal happens-after the sibling's invalidate
    // store and this load sees it. A failure on a still-valid block (where the
    // re-check returns false) propagates unchanged -- a real invariant
    // violation, not staleness.
    return HogDBError(HogDBCode::kStaleBlock);
  }
  return r;
}

void
BlockTX::unlock(const ItemKey &key, ItemChange &change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {
    auto *lock = addr_balance_locks_.get(key.get_address());
    CHECK_PTR(lock);
    LockContext ctx(&db_, block_height_, key, &cost_model_);
    do_unlock_item(*lock, change, ctx);
    return;
  }

  auto *lock = item_locks_.get(key);
  CHECK_PTR(lock);
  LockContext ctx(&db_, block_height_, key, &cost_model_);
  do_unlock_item(*lock, change, ctx);
}

HogDBStatus<int128_t>
BlockTX::commit_change(const ItemKey &key, ItemChange change)
{
  // TODO(geoff): In many cases we can evaluate here whether or not the item
  // change commit_change is a no-op.  In such cases, we can avoid acquiring the
  // lock.
  if (key.is_address_balance()) {
    auto *lock = addr_balance_locks_.get(key.get_address());
    CHECK_PTR(lock);
    LockContext ctx(&db_, block_height_, key, &cost_model_);
    auto r = lock->commit_change(std::move(change), ctx);
    if (!r.has_value()) {
      return r.error();
    }
    return 0;
  }
  auto *lock = item_locks_.get(key);
  CHECK_PTR(lock);

  LockContext ctx(&db_, block_height_, key, &cost_model_);
  return lock->commit_change(std::move(change), ctx);
}

HogDBStatus<std::optional<QueueEntry>>
BlockTX::get_concurrent_rcq_entry(const ItemKey &key, uint32_t index)
{
  auto *lock = item_locks_.get(key);
  if (!lock) {
    // Key was deleted and recreated as an RCQ in the same tx, or the lock
    // doesn't exist yet. A freshly-created queue has no concurrent entries.
    return std::nullopt;
  }
  return lock->get_concurrent_entry(index);
}

HogDBStatus<int128_t>
BlockTX::read_nnint128_base(const ItemKey &key)
{
  if (auto s = check_stale(); !s) [[unlikely]]
    return s.error();
  auto *lock = item_locks_.get(key);
  if (!lock) {
    if (!key.is_contract_data()) {
      auto r = db_.read_nnint128(key);
      if (!r) {
        if (r.error() == HogDBCode::kNotFound ||
            r.error() == HogDBCode::kTypeMismatch) {
          return 0;
        }
        return r.error();
      }
      return *r;
    }

    auto snapshot = get_snapshot_cost_and_cache_data(key);
    if (!snapshot) {
      return snapshot.error();
    }
    lock = item_locks_.insert_or_get(key, *snapshot);
    CHECK_PTR(lock);
  }

  auto base = lock->get_cached_nnint128_base();
  if (base) {
    return *base;
  }
  return 0;
}

HogDBStatus<int128_t>
BlockTX::read_balance_base(const Address &addr)
{
  if (auto s = check_stale(); !s) [[unlikely]]
    return s.error();
  auto *lock = addr_balance_locks_.get(addr);
  if (!lock) {
    return db_.read_address_balance(addr);
  }
  auto base = lock->get_cached_nnint128_base();
  return base.has_value() ? *base : 0;
}

HogDBStatus<void>
BlockTX::commit(WorkerPool &pool)
{
  if (is_poisoned()) {
    return HogDBError(HogDBCode::kInternal);
  }
  if (!changeset_.has_value() ||
      mode_ == BlockExecutionMode::kBlockExecutionModeViewing) {
    return HogDBError(mode_ == BlockExecutionMode::kBlockExecutionModeViewing
                          ? HogDBCode::kInvalidExecutorBehavior
                          : HogDBCode::kInternal);
  }
  if (auto r = db_.apply(*changeset_, pool); !r) {
    return r.error();
  }
  changeset_.reset();

  // Extract auth/balance data (parallel destructive, no DAO writes).
  PerThread<KeyChangeBatch> key_changes;

  item_locks_.for_each_parallel_destructive_among_workers(
      [&key_changes, this](std::pair<const ItemKey &, const ItemLock &> v) {
        if (!v.second.is_finalized()) {
          return;
        }

        if (v.first.is_address_auth_data()) {
          if (auto const *inner = v.second.get_inner<PointerLock<AuthLock>>()) {
            key_changes->emplace_back(
                v.first.to_contract_address(),
                x23::Ptr<AddressAuthData>(new AddressAuthData(
                    inner->get()->build_result(block_height_))));
          }
          else if (v.second.get_inner<DeletionLock>()) {
            key_changes->emplace_back(v.first.to_contract_address(), nullptr);
          }
          else {
            panic("unsupported type at auth data key: {}", v.first);
          }
        }
      },
      pool);

  PerThread<std::vector<Delta>> balance_deltas;

  addr_balance_locks_.for_each_parallel_destructive_among_workers(
      [&balance_deltas, this](std::pair<const Address &, const ItemLock &> v) {
        if (!v.second.is_finalized()) {
          return;
        }

        auto const *inner = v.second.get_inner<BalanceLock>();
        CHECK(inner != nullptr);

        // Forward the committed delta (which accounts for the storage cost
        // charged when a balance is created for the first time), not the raw
        // delta — otherwise the scheduler would observe a value larger than the
        // balance actually committed to the DB by the balance creation cost.
        auto value = inner->get_committed_delta(
            LockContext(&db_, block_height_,
                        make_address_balance_item_key(v.first), &cost_model_));

        if (value == 0) {
          return;
        }

        balance_deltas->push_back(
            Delta{.address = v.first.get_raw(), .hog_delta = to_xdr(value)});
      },
      pool);

  background_commit_worker_.submit_job(block_height_, std::move(key_changes),
                                       std::move(balance_deltas));

  return {};
}

HogDBStatus<void>
BlockTX::close_skip_hash_for_testing()
{
  if (is_poisoned()) {
    return HogDBError(HogDBCode::kInternal);
  }
  ChangeSet changes = db_.create_changeset(/*compute_hash=*/false);
  if (auto r = stage_locks(changes); !r) {
    return r;
  }
  changeset_ = std::move(changes);
  return {};
}

HogDBStatus<DbHash>
BlockTX::close(const DbHash &base_hash)
{
  if (is_poisoned()) {
    return HogDBError(HogDBCode::kInternal);
  }
  ChangeSet changes = db_.create_changeset(/*compute_hash=*/true);
  if (auto r = stage_locks(changes); !r) {
    return r.error();
  }

  LtHash16 delta = changes.close();
  changeset_ = std::move(changes);

  LtHash16 final_hash = lthash16_from_db_hash(base_hash);
  final_hash += delta;
  return db_hash_from_lthash16(final_hash);
}

HogDBStatus<void>
BlockTX::stage_locks(ChangeSet &changes)
{
  std::mutex err_lock;
  HogDBStatus<void> status;

  item_locks_.for_each_parallel_among_workers(
      [&](std::pair<const ItemKey &, ItemLock &> v) {
        if (!v.second.is_finalized()) {
          return;
        }
        auto r = v.second.stage_committed_changes(
            LockContext(&db_, block_height_, v.first, &cost_model_), changes);
        if (!r) [[unlikely]] {
          std::scoped_lock guard(err_lock);
          status = r;
        }
      });

  if (!status) {
    return status;
  }

  addr_balance_locks_.for_each_parallel_among_workers(
      [&](std::pair<const Address &, ItemLock &> v) {
        if (!v.second.is_finalized()) {
          return;
        }
        auto r = v.second.stage_committed_changes(
            LockContext(&db_, block_height_,
                        make_address_balance_item_key(v.first), &cost_model_),
            changes);
        if (!r) [[unlikely]] {
          std::scoped_lock guard(err_lock);
          status = r;
        }
      });

  return status;
}

} // namespace hogd
