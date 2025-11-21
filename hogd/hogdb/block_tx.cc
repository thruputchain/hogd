#include "hogd/hogdb/block_tx.h"

#include <cassert>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "account.hh"
#include "auth_rpc.hh"
#include "hogd/base/bits.h"
#include "hogd/base/compiler.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock.h"
#include "hogd/hogdb/item_lock/deletion_lock.h"
#include "hogd/hogdb/item_lock/nnint128_lock.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "sched_db.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/x23.h"

namespace hogd {

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<int128_t>
BlockTX::do_lock(ItemLock &lock, const ItemChange &change,
                 const StorageCostModel *cost_model)
{
  return lock.try_atomic_change_lock(nullptr, &change, cost_model);
}

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<int128_t>
BlockTX::do_modify_lock(ItemLock &lock, const ItemChange &old_change,
                        const ItemChange &new_change,
                        const StorageCostModel *cost_model)
{
  return lock.try_atomic_change_lock(&old_change, &new_change, cost_model);
}

ATTRIBUTE_ALWAYS_INLINE void
BlockTX::do_unlock_item(ItemLock &lock, const ItemChange &change,
                        const StorageCostModel *cost_model)
{
  auto r = lock.try_atomic_change_lock(&change, nullptr, cost_model);
  CHECK(r.has_value());
}

void
BlockTX::release_balance(const Address &addr, int128_t delta)
{
  assert(delta > 0);

  auto *lock = addr_balance_locks_.insert_or_get(addr, [&] {
    return get_snapshot_cost_and_cache_data(
        make_address_balance_item_key(addr));
  });
  CHECK_PTR(lock);

  auto base = read_balance_base(addr);
  CHECK(base.has_value());

  ItemChange change = ItemChange(NNInt128(*base, delta));

  if (auto r = do_lock(*lock, change, &cost_model_); !r) [[unlikely]] {
    throw std::runtime_error("failed to lock balance");
  }

  lock->commit_change(change, &cost_model_);
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

    return std::make_pair(0, ItemLockCacheData{kItemLockCacheDataTypeNone});
  }
  return *snapshot_cost;
}

HogDBStatus<int128_t>
BlockTX::lock(const ItemKey &key, ItemChange &change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {

    auto *lock = addr_balance_locks_.insert_or_get(key.get_address(), [&] {
      return get_snapshot_cost_and_cache_data(
          make_address_balance_item_key(key.get_address()));
    });

    return do_lock(*lock, change, &cost_model_)
        .and_then([](auto /*unused*/) -> HogDBStatus<int128_t> { return 0; });
  }

  auto *item = item_locks_.insert_or_get(key, [&] {
    return get_snapshot_cost_and_cache_data(key);
  });

  assert(item != nullptr);

  return do_lock(*item, change, &cost_model_);
}

HogDBStatus<int128_t>
BlockTX::modify_lock(const ItemKey &key, const ItemChange &old_change,
                     const ItemChange &new_change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {
    auto *lock = addr_balance_locks_.get(key.get_address());
    CHECK_PTR(lock);
    return do_modify_lock(*lock, old_change, new_change, &cost_model_)
        .and_then([](auto /*unused*/) -> HogDBStatus<int128_t> { return 0; });
  }

  auto *lock = item_locks_.get(key);
  CHECK_PTR(lock);

  return do_modify_lock(*lock, old_change, new_change, &cost_model_);
}

void
BlockTX::unlock(const ItemKey &key, ItemChange &change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {
    auto *lock = addr_balance_locks_.get(key.get_address());
    CHECK_PTR(lock);
    do_unlock_item(*lock, change, &cost_model_);
    return;
  }

  auto *lock = item_locks_.get(key);
  CHECK_PTR(lock);
  do_unlock_item(*lock, change, &cost_model_);
}

int128_t
BlockTX::commit_change(const ItemKey &key, ItemChange const &change)
{
  // TODO(geoff): In many cases we can evaluate here whether or not the item
  // change commit_change is a no-op.  In such cases, we can avoid acquiring the
  // lock.
  if (key.is_address_balance()) {
    auto *lock = addr_balance_locks_.get(key.get_address());
    CHECK_PTR(lock);
    lock->commit_change(change, &cost_model_);
    return 0;
  }
  auto *lock = item_locks_.get(key);
  CHECK_PTR(lock);

  return lock->commit_change(change, &cost_model_);
}

HogDBStatus<int128_t>
BlockTX::read_nnint128_base(const ItemKey &key)
{
  auto *lock = item_locks_.get(key);
  if (!lock) {
    return db_.read_nnint128(key);
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
  auto *lock = addr_balance_locks_.get(addr);
  if (!lock) {
    return db_.read_address_balance(addr);
  }
  auto base = lock->get_cached_nnint128_base();
  return base.has_value() ? *base : 0;
}

namespace {
x23::Detached
async_dispatch_balance_deltas(
    BlockHeight block_height, PerThread<std::vector<Delta>> balance_deltas,
    std::shared_ptr<BlockTX::sched_db_client_t> sched_db_client)
{
  co_await default_threadpool().bg();

  HogsTransferred transferred{.block = block_height, .deltas = {}};
  auto ul = balance_deltas.lock_threads();
  balance_deltas.for_each(ul, [&](const std::vector<Delta> &deltas) {
    transferred.deltas.append_range(deltas);
  });
  co_await sched_db_client->scheddb_hogs_transferred(std::move(transferred));
  co_return;
}
} // namespace

HogDBStatus<void>
BlockTX::commit(WorkerPool &pool)
{
  std::mutex lock;
  HogDBStatus<void> ret;

  PerThread<KeyChangeBatch> key_changes;

  // write item changes.
  item_locks_.for_each_parallel_destructive(
      [this, &lock, &ret,
       &key_changes](std::pair<const ItemKey &, const ItemLock &> v) {
        if (!v.second.is_finalized()) {
          return;
        }
        HogDBStatus<void> tmp =
            v.second.write_committed_change(v.first, db_, trie_);
        if (!tmp) [[unlikely]] {
          std::scoped_lock guard(lock);
          ret = tmp;
        }

        if (v.first.is_account_auth_data()) {
          if (auto const *inner = v.second.get_inner<PointerLock<AuthLock>>()) {
            AccountAuthData auth_data = inner->get()->build_result();
            key_changes->emplace_back(v.first.to_contract_address(),
                                      x23::Ptr<AccountAuthData>(
                                          new AccountAuthData(
                                              inner->get()->build_result())));
          }
          else if (v.second.get_inner<DeletionLock>()) {
            key_changes->emplace_back(v.first.to_contract_address(), nullptr);
          }
          else {
            panic("unsupported type at auth data key: {}", v.first);
          }
        }
      },
      1024, pool);

  PerThread<std::vector<Delta>> balance_deltas;

  // write address balances.
  addr_balance_locks_.for_each_parallel_destructive(
      [this, &lock, &ret,
       &balance_deltas](std::pair<const Address &, const ItemLock &> v) {
        if (!v.second.is_finalized()) {
          return;
        }
        const auto *inner = v.second.get_inner<NNInt128Lock>();
        CHECK(inner != nullptr);
        auto [base, delta] = inner->get_base_and_delta();
        auto [new_val, of] = overflowing_add(base, delta);
        assert(!of);
        HogDBStatus<void> tmp = db_.write_address_balance(v.first, new_val);
        if (!tmp) [[unlikely]] {
          std::scoped_lock guard(lock);
          ret = tmp;
        }

        balance_deltas->push_back(
            Delta{.address = v.first.get_raw(), .hog_delta = to_xdr(delta)});

        // write the address balance to the trie
        // TODO(geoff,jhumphri): should the value type here be byte_view(little
        // endian)? Right now, the type is kNNInt128 and the value is
        // byte_view(little endian) because that is what is written to the db in
        // write_address_balance. We should factor this out into one method,
        // instead of requiring duplication of this logic.
        CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(
                  get_trie(), make_address_balance_item_key(v.first), kNNInt128,
                  byte_view(cpu_to_le(new_val)))
                  .has_value());
      },
      1024, pool);

  if (sched_db_client_) {
    async_dispatch_balance_deltas(block_height_, std::move(balance_deltas),
                                  sched_db_client_);
  }

  auth_change_subscription_manager_.detached_publish_auth_changes(
      block_height_, std::move(key_changes));
  return ret;
}

} // namespace hogd
