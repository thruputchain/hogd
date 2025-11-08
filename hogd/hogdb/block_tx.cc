#include "hogd/hogdb/block_tx.h"

#include <cassert>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

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
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "sched_db.hh"
#include "types.hh"
#include "x23/async.h"

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
BlockTX::do_unlock_item(ItemLock &lock,
                        const ItemChange &change, const StorageCostModel *cost_model)
{
  lock.try_atomic_change_lock(&change, nullptr, cost_model);
}

void
BlockTX::release_balance(const Address &addr, int128_t delta)
{
  assert(delta > 0);

  HogDBStatus<ItemChange> change = make_balance_change(db_, addr, delta);
  if (!change) [[unlikely]] {
    throw std::runtime_error("failed to make balance change");
  }

  auto *lock = addr_balance_locks_.get(addr);
  CHECK_PTR(lock);

  if (auto r = do_lock(*lock, *change, &cost_model_); !r) [[unlikely]] {
    throw std::runtime_error("failed to lock balance");
  }
}

HogDBStatus<int128_t>
BlockTX::get_snapshot_cost(const ItemKey &key)
{
  if (key.is_address_balance()) {
    return 0;
  }
  auto snapshot_cost = db_.get_charged_storage_cost(key, &cost_model_);
  if (!snapshot_cost) {
    if (snapshot_cost.error() != HogDBCode::kNotFound) {
      return snapshot_cost.error();
    }
    return 0;
  }
  return *snapshot_cost;
}

HogDBStatus<int128_t>
BlockTX::lock(const ItemKey &key, ItemChange &change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {

    auto *lock = addr_balance_locks_.insert_or_get(key.get_address(), 0);


    return do_lock(*lock, change, &cost_model_)
        .and_then([](auto /*unused*/) -> HogDBStatus<int128_t> { return 0; });
  }

  auto *item =
      item_locks_.insert_or_get(key, [&] { return get_snapshot_cost(key); });

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
    std::unique_lock<std::mutex> ul(get_mutex(key));
    return do_modify_lock(ul, *lock, old_change, new_change, &cost_model_)
        .and_then([](auto /*unused*/) -> HogDBStatus<int128_t> { return 0; });
  }

  auto *lock = item_locks_.get(key);
  CHECK_PTR(lock);
  auto ul = std::unique_lock<std::mutex>(get_mutex(key));
  if (!lock->change.has_snapshot_cost()) [[unlikely]] {
    return HogDBError{HogDBCode::kInternal};
  }
  return do_modify_lock(ul, *lock, old_change, new_change, &cost_model_);
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
  item_locks_.for_each_parallel_destructive(
      [this, &lock, &ret](std::pair<const ItemKey &, const ItemLock &> v) {
        if (!v.second.is_finalized()) {
          return;
        }
        HogDBStatus<void> tmp = v.second.write_committed_change(v.first, db_, trie_);
        if (!tmp) [[unlikely]] {
          std::scoped_lock guard(lock);
          ret = tmp;
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
        assert(v.second.change.get_raw_nnint128().has_value());
        auto raw_nnint128 = v.second.change.get_raw_nnint128().value();
        int128_t prev_val = raw_nnint128.base();
        int128_t new_val = v.second.change.to_nnint128().value();
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
      },
      1024, pool);

  if (sched_db_client_) {
    async_dispatch_balance_deltas(block_height_, std::move(balance_deltas),
                                  sched_db_client_);
  }

  return ret;
}

} // namespace hogd
