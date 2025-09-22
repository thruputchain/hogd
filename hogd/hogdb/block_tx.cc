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

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<void>
BlockTX::do_lock([[maybe_unused]] const std::unique_lock<std::mutex> &ul,
                 ItemLock &lock, const ItemChange &change)
{
  assert(ul.owns_lock());

  if (++lock.ref == 1) {
    lock.change = change;
    return {};
  }

  if (HogDBStatus<void> ret = lock.change.merge(change); !ret) {
    lock.ref--;
    return ret.error();
  }

  return {};
}

ATTRIBUTE_ALWAYS_INLINE HogDBStatus<void>
BlockTX::do_modify_lock([[maybe_unused]] const std::unique_lock<std::mutex> &ul,
                        ItemLock &lock, const ItemChange &old_change,
                        const ItemChange &new_change)
{
  assert(ul.owns_lock());
  assert(lock.ref > 0);

  if (lock.ref == 1) {
    lock.change = new_change;
    return {};
  }

  ItemChange tmp = lock.change;
  tmp.undo_merge(old_change);
  HogDBStatus<void> ret = tmp.merge(new_change);
  if (!ret)
    return ret.error();

  lock.change = std::move(tmp);
  return {};
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
  HogDBStatus<ItemChange> change = make_balance_change(db_, addr, delta);
  if (!change)
    return change.error();
  auto [lock, ul] = addr_balance_locks_[addr];
  return do_lock(ul, lock, *change);
}

HogDBStatus<void>
BlockTX::modify_balance(const Address &addr, int128_t old_delta,
                        int128_t new_delta)
{
  auto [lock, ul] = addr_balance_locks_[addr];
  std::optional<NNInt128> nnint = lock.change.get_raw_nnint128();
  if (!nnint) [[unlikely]]
    throw std::runtime_error("locked balance not a nnint128");

  // TODO(geoff): among the other things that need to be changed here is that
  // this doesn't guarantee that you can always rollback to a savepoint
  ItemChange old_change(NNInt128(nnint->base(), old_delta));
  ItemChange new_change(NNInt128(nnint->base(), new_delta));
  return do_modify_lock(ul, lock, old_change, new_change);
}

void
BlockTX::unlock_balance(const Address &addr, int128_t delta)
{
  auto [lock, ul] = addr_balance_locks_[addr];
  std::optional<NNInt128> nnint = lock.change.get_raw_nnint128();
  if (!nnint) [[unlikely]]
    throw std::runtime_error("locked balance not a nnint128");

  ItemChange change(NNInt128(nnint->base(), delta));
  do_unlock_addr_balance(ul, addr, lock, change);
}

HogDBStatus<void>
BlockTX::lock(const ItemKey &key, ItemChange &change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {
    auto [lock, ul] = addr_balance_locks_[key.get_address()];
    return do_lock(ul, lock, change);
  }

  // calculate storage cost.
  HogDBStatus<ssize_t> delta = change.storage_cost_delta(db_, key);
  if (!delta) [[unlikely]]
    return delta.error();

  // take the lock and charge storage cost.
  auto [lock, ul] = item_locks_[key];
  if (lock.ref == 0 && *delta != 0) {
    HogDBStatus<void> ret = lock_balance(key.get_address(), -*delta);
    if (!ret)
      return ret.error();
  }
  return do_lock(ul, lock, change);
}

HogDBStatus<void>
BlockTX::modify_lock(const ItemKey &key, ItemChange &old_change,
                     ItemChange &new_change)
{
  // no storage cost for address balances.
  if (key.is_address_balance()) {
    auto [lock, ul] = addr_balance_locks_[key.get_address()];
    return do_modify_lock(ul, lock, old_change, new_change);
  }

  // calculate difference in storage cost.
  HogDBStatus<ssize_t> old_delta = old_change.storage_cost_delta(db_, key);
  if (!old_delta) [[unlikely]]
    return old_delta.error();
  HogDBStatus<ssize_t> new_delta = new_change.storage_cost_delta(db_, key);
  if (!new_delta) [[unlikely]]
    return new_delta.error();

  // modify the lock and the storage cost.
  // TODO(geoff): The limitation here is that this design assumes storage costs
  // can only change if we are the only tx modifying the item.
  // This is true for bytestrings and nnints; but not necessarily for all data
  // types.
  // TODO(geoff): As it is now, there is no workflow with bytestrings or nnints
  // in which we could successfully modify the balance but then do_modify_lock
  // fails. That may be added later -- if so, then we need to rework this
  // workflow.
  auto [lock, ul] = item_locks_[key];
  if (lock.ref == 1 && *old_delta != *new_delta) {
    // Cost delta reflects the change in storage _cost_, but we are changing
    // here the amount of remaining available balance (i.e. -cost).
    HogDBStatus<void> ret =
        modify_balance(key.get_address(), -*old_delta, -*new_delta);
    if (!ret)
      return ret.error();
  }

  // @abelay left the assertion as
  // assert(lock.ref == 1 || *old_delta == *new_delta);
  // For the bytestring type, this assertion could be triggered
  // if multiple txs set a bytestring value and then one of those
  // tries to set a different value (while the other txs still hold the
  // reference). The behavior is still correct in that case, as do_modify_lock
  // will fail.

  auto res = do_modify_lock(ul, lock, old_change, new_change);
  assert(lock.ref == 1 || *old_delta == *new_delta || !res.has_value());
  return res;
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

  // calculate storage cost.
  HogDBStatus<ssize_t> delta = change.storage_cost_delta(db_, key);
  if (!delta) [[unlikely]]
    throw std::runtime_error("storage delta cannot fail once locked.");

  // release the lock and refund storage cost.
  auto [lock, ul] = item_locks_[key];
  if (lock.ref == 1 && *delta != 0)
    unlock_balance(key.get_address(), -*delta);
  do_unlock_item(ul, key, lock, change);
}

void
BlockTX::commit_change(const ItemKey &key, ItemChange const &change)
{
  // TODO(geoff): In many cases we can evaluate here whether or not the item
  // change commit_change is a no-op.  In such cases, we can avoid acquiring the
  // lock.
  if (key.is_address_balance()) {
    auto [lock, ul] = addr_balance_locks_[key.get_address()];
    lock.change.commit_change(change);
    return;
  }
  auto [lock, ul] = item_locks_[key];
  lock.change.commit_change(change);
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
