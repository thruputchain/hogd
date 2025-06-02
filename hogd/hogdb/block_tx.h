// block_tx.h - transaction handling logic at the granularity of a block.

#pragma once

#include <utility>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"

namespace hogd {

// BlockTX manages a set of pending changes made in a block relative to the
// current snapshot state. They can be applied to the ramdb via commit().
// Multiple BlockTXes can be instantiated, but only one should be committed.
class BlockTX {
public:
  BlockTX(HogDatabase &db) : db_(db) {}
  ~BlockTX() = default;

  // getters
  HogDatabase &get_db() const { return db_; }

  // commit applies all changes to the database.
  HogDBStatus<void> commit();
  // abort drops all changes.
  void abort() { locks_.clear(); }

  // lock reserves the right to apply change to an item.
  HogDBStatus<void> lock(ItemKey key, const ItemChange &change);
  // modify_lock modifies an existing change to an item that was reserved with
  // lock(). It could still fail if new_change has a conflict.
  HogDBStatus<void> modify_lock(ItemKey key, const ItemChange &old_change,
                                const ItemChange &new_change);
  // unlock unreserves the right to apply change to an item and reverts it.
  void unlock(ItemKey key, const ItemChange &change);

private:
  struct ItemLock {
    ItemChange change;
    size_t ref;
  };

  HogDatabase &db_;
  SynchronizedMap<ItemKey, ItemLock> locks_;
};

inline HogDBStatus<void>
BlockTX::lock(ItemKey key, const ItemChange &change)
{
  auto [val, ul] = locks_[key];
  if (++val.ref == 1) {
    val.change = change;
    return {};
  }

  if (HogDBStatus<void> ret = val.change.merge(change); !ret) {
    val.ref--;
    return ret.error();
  }

  return {};
}

inline void
BlockTX::unlock(ItemKey key, const ItemChange &change)
{
  auto [val, ul] = locks_[key];
  assert(val.ref > 0);
  if (--val.ref == 0) {
    locks_.remove(key, ul);
    return;
  }
  val.change.undo_merge(change);
}

} // namespace hogd
