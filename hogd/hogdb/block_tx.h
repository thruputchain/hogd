// block_tx.h - transaction handling logic at the granularity of a block.

#pragma once

#include <optional>
#include <utility>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/container/synchronized_map.h"
#include "hogd/hogdb/db.h"
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

  //
  // The read_* methods below read the current state of an item in the block
  // view. They will return kTypeMismatch if the stored type is different from
  // the requested type or kNotFound if the item does not exist.
  //

  HogDBStatus<int128_t> read_nnint128(ItemKey key);
  HogDBStatus<OwningSpan<const std::byte>> read_bytes(ItemKey key);

  // lock reserves the right to apply change to an item. If exclusive is false,
  // More than one change can be made to an item if they commute.
  HogDBStatus<void> lock(ItemKey key, const ItemChange &change,
                         bool exclusive = false);
  // modify_lock modifies an existing change to an item that was reserved with
  // lock(). It could still fail if new_change has a conflict.
  HogDBStatus<void> modify_lock(ItemKey key, const ItemChange &old_change,
                                const ItemChange &new_change);
  // unlock unreserves the right to apply change to an item and reverts it.
  void unlock(ItemKey key, const ItemChange &change);

  // commit applies all changes to the database.
  HogDBStatus<void> commit();

private:
  struct ItemLock {
    ItemChange change;
    bool exclusive;
    size_t ref;
  };

  HogDatabase &db_;
  SynchronizedMap<ItemKey, ItemLock> locks_;
};

inline HogDBStatus<int128_t>
BlockTX::read_nnint128(ItemKey key)
{
  auto [val, ul] = locks_.find(key);
  if (!val)
    return db_.read_nnint128(key);
  if (val->change.is_deletion())
    return HogDBError(HogDBCode::kNotFound);
  std::optional ret = val->change.to_nnint128();
  if (!ret)
    return HogDBError(HogDBCode::kTypeMismatch);
  return *ret;
}

inline HogDBStatus<OwningSpan<const std::byte>>
BlockTX::read_bytes(ItemKey key)
{
  auto [val, ul] = locks_.find(key);
  if (!val)
    return db_.read_bytes(key);
  if (val->change.is_deletion())
    return HogDBError(HogDBCode::kNotFound);
  OwningSpan<const std::byte> ret = val->change.to_bytes();
  if (!ret)
    return HogDBError(HogDBCode::kTypeMismatch);
  return ret;
}

inline HogDBStatus<void>
BlockTX::lock(ItemKey key, const ItemChange &change, bool exclusive)
{
  auto [val, ul] = locks_[key];
  if ((val.exclusive || exclusive) && val.ref > 0)
    return HogDBError(HogDBCode::kConflict);
  val.ref++;
  val.exclusive = exclusive;
  return val.change.merge(change);
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
