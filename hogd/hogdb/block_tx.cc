#include "hogd/hogdb/block_tx.h"

#include <cassert>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "hogd/base/bits.h"
#include "hogd/base/compiler.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/perthread.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "sched_db.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<int128_t>
ItemLockMVar::do_lock([[maybe_unused]] const std::unique_lock<std::mutex> &ul,
                      ItemLock &lock, const ItemChange &change,
                      const StorageCostModel *cost_model)
{
  assert(ul.owns_lock());
  assert(lock.ref >= 0);

  if (++lock.ref == 1) {
    lock.change = change;
    return lock.change.marginal_storage_cost(cost_model);
  }

  HogDBStatus<int128_t> ret = lock.change.merge(change, cost_model);
  if (!ret) {
    assert(lock.ref > 0);
    lock.ref--;
    return ret.error();
  }

  return ret;
}

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<int128_t>
ItemLockMVar::do_modify_lock(
    [[maybe_unused]] const std::unique_lock<std::mutex> &ul, ItemLock &lock,
    const ItemChange &old_change, const ItemChange &new_change,
    const StorageCostModel *cost_model)
{
  assert(ul.owns_lock());
  assert(lock.ref > 0);

  if (lock.ref == 1) {
    lock.change = new_change;
    return lock.change.marginal_storage_cost(cost_model);
  }

  ItemChange tmp = lock.change;
  tmp.undo_merge(old_change);
  HogDBStatus<int128_t> ret = tmp.merge(new_change, cost_model);
  if (!ret)
    return ret.error();

  lock.change = std::move(tmp);
  return ret;
}

ATTRIBUTE_ALWAYS_INLINE void
ItemLockMVar::do_unlock(const std::unique_lock<std::mutex> &ul, ItemLock &lock,
                        const ItemChange &change)
{
  assert(ul.owns_lock());
  assert(lock.ref > 0);

  if (--lock.ref == 0) {
    // No need to remove the lock from the map;
    // if the key is ever acquired again later, the item change will be
    // overwritten.
    return;
  }
  lock.change.undo_merge(change);
}

HogDBStatus<int128_t>
ItemLockMVar::lock(ItemChange &change)
{
  auto [lock, ul] = acquire_local_mutex();
  return do_lock(ul, *lock, change, cost_model_);
}

HogDBStatus<int128_t>
ItemLockMVar::modify_lock(const ItemKey &key, const ItemChange &old_change,
                          const ItemChange &new_change)
{
  auto [lock, ul] = acquire_local_mutex();
  return do_modify_lock(ul, *lock, old_change, new_change, cost_model_)
      .and_then([&key](int128_t released_cost) -> HogDBStatus<int128_t> {
        if (key.is_address_balance()) {
          return 0;
        }
        return released_cost;
      });
}

void
ItemLockMVar::unlock(const ItemChange &change)
{
  auto [lock, ul] = acquire_local_mutex();
  do_unlock(ul, *lock, change);
}

int128_t
ItemLockMVar::commit_change(const ItemKey &key, const ItemChange &change)
{
  auto [lock, ul] = acquire_local_mutex();
  if (key.is_address_balance()) {
    lock->change.commit_change(change);
    return 0;
  }
  int128_t cost_before = lock->change.committed_storage_cost(cost_model_);
  lock->change.commit_change(change);
  int128_t cost_after = lock->change.committed_storage_cost(cost_model_);
  return cost_after - cost_before;
}

HogDBStatus<ItemLockMVar *>
BlockTX::get_item_lock(const ItemKey &key)
{

  if (key.is_address_balance()) {
    auto [lock, ul] = addr_balance_locks_.find(key.get_address());
    if (lock) {
      return lock;
    }
    assert(lock == nullptr);

    auto *out =
        addr_balance_locks_.insert(key.get_address(),
                                   ItemLockMVar(*ul.mutex(), &cost_model_), ul);
    out->unsafe_set_snaphot_cost(0);
    return out;
  }

  auto [lock, ul] = item_locks_.find(key);
  if (lock) {
    return lock;
  }
  assert(lock == nullptr);

  auto *out =
      item_locks_.insert(key, ItemLockMVar(*ul.mutex(), &cost_model_), ul);
  if (auto r = out->unsafe_load_snapshot_cost(db_, key); !r) {
    return r.error();
  }
  return out;
}

namespace {
x23::Detached
async_dispatch_balance_deltas(BlockHeight block_height,
                              PerThread<std::vector<Delta>> balance_deltas,
                              auto sched_db_client)
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

  // write item changes.
  item_locks_.for_each_parallel(
      [this, &lock, &ret](std::pair<const ItemKey, const ItemLockMVar &> v) {
        const ItemChange *change = v.second.unsafe_get_change();
        if (!change) {
          return;
        }
        HogDBStatus<void> tmp = change->commit(db_, trie_, v.first);
        if (!tmp) [[unlikely]] {
          std::scoped_lock guard(lock);
          ret = tmp;
        }
      },
      kDefaultParallelRangeWidth, pool);

  PerThread<std::vector<Delta>> balance_deltas;

  // write address balances.
  addr_balance_locks_.for_each_parallel(
      [this, &lock, &ret,
       &balance_deltas](std::pair<const Address, const ItemLockMVar &> v) {
        const ItemChange *change = v.second.unsafe_get_change();
        if (!change) {
          return;
        }
        assert(change->get_raw_nnint128().has_value());
        auto raw_nnint128 = change->get_raw_nnint128().value();
        int128_t prev_val = raw_nnint128.base();
        int128_t new_val = change->to_nnint128().value();
        HogDBStatus<void> tmp = db_.write_address_balance(v.first, new_val);
        if (!tmp) [[unlikely]] {
          std::scoped_lock guard(lock);
          ret = tmp;
        }

        auto [delta, of] = overflowing_sub(new_val, prev_val);
        assert(!of);
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
      });

  if (sched_db_client_) {
    async_dispatch_balance_deltas(block_height_, std::move(balance_deltas),
                                  sched_db_client_);
  }

  return ret;
}

} // namespace hogd
