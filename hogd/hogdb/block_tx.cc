#include "hogd/hogdb/block_tx.h"

#include <cassert>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "hogd/base/compiler.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"

namespace hogd {

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<int128_t>
BlockTX::do_lock([[maybe_unused]] const std::unique_lock<std::mutex> &ul,
                 ItemLock &lock, const ItemChange &change)
{
  assert(ul.owns_lock());

  if (++lock.ref == 1) {
    lock.change = change;
    return lock.change.marginal_storage_cost();
  }

  HogDBStatus<int128_t> ret = lock.change.merge(change);
  if (!ret) {
    lock.ref--;
    return ret.error();
  }

  return ret;
}

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<int128_t>
BlockTX::do_modify_lock([[maybe_unused]] const std::unique_lock<std::mutex> &ul,
                        ItemLock &lock, const ItemChange &old_change,
                        const ItemChange &new_change)
{
  assert(ul.owns_lock());
  assert(lock.ref > 0);

  info("modifying lock");
  if (lock.ref == 1) {
    lock.change = new_change;
    info("successfuly modified lock single ref");
    return lock.change.marginal_storage_cost();
  }

  ItemChange tmp = lock.change;
  tmp.undo_merge(old_change);
  HogDBStatus<int128_t> ret = tmp.merge(new_change);
  if (!ret)
    return ret.error();

  lock.change = std::move(tmp);
  info("successfuly modified lock");
  return ret;
}

ATTRIBUTE_ALWAYS_INLINE void
BlockTX::do_unlock_item(const std::unique_lock<std::mutex> &ul,
                        const ItemKey &key, ItemLock &lock,
                        const ItemChange &change)
{
  assert(ul.owns_lock());
  assert(lock.ref > 0);

  if (--lock.ref == 0) {
    item_locks_.remove(key, ul);
    return;
  }
  lock.change.undo_merge(change);
}

ATTRIBUTE_ALWAYS_INLINE void
BlockTX::do_unlock_addr_balance(const std::unique_lock<std::mutex> &ul,
                                const Address &addr, ItemLock &lock,
                                const ItemChange &change)
{
  assert(ul.owns_lock());
  assert(lock.ref > 0);

  if (--lock.ref == 0) {
    addr_balance_locks_.remove(addr, ul);
    return;
  }
  lock.change.undo_merge(change);
}

HogDBStatus<void>
BlockTX::lock_balance(const Address &addr, int128_t delta)
{
  info("locking balance for addr {} delta {}", addr, delta);
  HogDBStatus<ItemChange> change = make_balance_change(db_, addr, delta);
  if (!change)
    return change.error();
  change->set_snapshot_cost(0);
  auto [lock, ul] = addr_balance_locks_[addr];
  return do_lock(ul, lock, *change)
      .and_then([](auto /*unused*/) -> HogDBStatus<void> { return {}; });
}

#if 0
HogDBStatus<void>
BlockTX::modify_balance(const Address &addr, int128_t old_delta,
                        int128_t new_delta)
{
  info("modifying balance for addr {} old_delta {} new_delta {}", addr, old_delta, new_delta);
  auto [lock, ul] = addr_balance_locks_[addr];
  std::optional<NNInt128> nnint = lock.change.get_raw_nnint128();
  if (!nnint) [[unlikely]]
    throw std::runtime_error("locked balance not a nnint128");

  ItemChange old_change(NNInt128(nnint->base(), old_delta));
  ItemChange new_change(NNInt128(nnint->base(), new_delta));
  return do_modify_lock(ul, lock, old_change, new_change)
      .and_then([](auto /*unused*/) -> HogDBStatus<void> { return {}; });
} 
#endif

void
BlockTX::release_balance(const Address &addr, int128_t delta)
{
  assert(delta > 0);
  info("releasing balance for addr {} delta {}", addr, delta);
  if (auto r = lock_balance(addr, delta); !r) [[unlikely]]
    throw std::runtime_error("failed to release balance");
}

HogDBStatus<int128_t>
BlockTX::lock(const ItemKey &key, ItemChange &change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {
    auto [lock, ul] = addr_balance_locks_[key.get_address()];
    change.set_snapshot_cost(0);
    return do_lock(ul, lock, change)
        .and_then([](auto /*unused*/) -> HogDBStatus<int128_t> { return 0; });
  }

  auto [lock, ul] = item_locks_[key];
  if (auto r = lock.change.load_snapshot_cost(db_, key); !r)
    return r.error();
  return do_lock(ul, lock, change);
}

HogDBStatus<int128_t>
BlockTX::modify_lock(const ItemKey &key, ItemChange &old_change,
                     ItemChange &new_change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {
    auto [lock, ul] = addr_balance_locks_[key.get_address()];
    return do_modify_lock(ul, lock, old_change, new_change)
        .and_then([](auto /*unused*/) -> HogDBStatus<int128_t> { return 0; });
  }

  auto [lock, ul] = item_locks_[key];
  return do_modify_lock(ul, lock, old_change, new_change);
}

void
BlockTX::unlock(const ItemKey &key, ItemChange &change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {
    auto [lock, ul] = addr_balance_locks_[key.get_address()];
    do_unlock_addr_balance(ul, key.get_address(), lock, change);
    return;
  }

  auto [lock, ul] = item_locks_[key];
  do_unlock_item(ul, key, lock, change);
}

int128_t
BlockTX::commit_change(const ItemKey &key, ItemChange const &change)
{
  // TODO(geoff): In many cases we can evaluate here whether or not the item
  // change commit_change is a no-op.  In such cases, we can avoid acquiring the
  // lock.
  if (key.is_address_balance()) {
    auto [lock, ul] = addr_balance_locks_[key.get_address()];
    lock.change.commit_change(change);
    return 0;
  }
  auto [lock, ul] = item_locks_[key];
  int128_t cost_before = lock.change.committed_storage_cost();
  info("cost_before for key {} = {}", key, cost_before);
  lock.change.commit_change(change);
  int128_t cost_after = lock.change.committed_storage_cost();
  info("cost_after for key {} = {}", key, cost_after);
  return cost_after - cost_before;
}

HogDBStatus<void>
BlockTX::commit(WorkerPool &pool)
{
  std::mutex lock;
  HogDBStatus<void> ret;

  // write item changes.
  item_locks_.for_each_parallel(
      [this, &lock, &ret](std::pair<const ItemKey, const ItemLock &> v) {
        HogDBStatus<void> tmp = v.second.change.commit(db_, trie_, v.first);
        if (!tmp) [[unlikely]] {
          std::scoped_lock guard(lock);
          ret = tmp;
        }
      },
      kDefaultParallelRangeWidth, pool);

  // write address balances.
  addr_balance_locks_.for_each_parallel(
      [this, &lock, &ret](std::pair<const Address, const ItemLock &> v) {
        int128_t val = v.second.change.to_nnint128().value();
        HogDBStatus<void> tmp = db_.write_address_balance(v.first, val);
        if (!tmp) [[unlikely]] {
          std::scoped_lock guard(lock);
          ret = tmp;
        }
      });

  return ret;
}

} // namespace hogd
