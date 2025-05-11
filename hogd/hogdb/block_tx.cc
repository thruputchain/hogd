#include "hogd/hogdb/block_tx.h"

#include <cassert>
#include <mutex>
#include <utility>

#include "hogd/container/synchronized_map.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<void>
BlockTX::modify_lock(ItemKey key, const ItemChange &old_change,
                     const ItemChange &new_change)
{
  auto [val, ul] = locks_[key];
  assert(val.ref > 0);
  if (val.ref == 1) {
    val.change = new_change;
    return {};
  }

  ItemChange tmp = val.change;
  tmp.undo_merge(old_change);
  HogDBStatus<void> ret = tmp.merge(new_change);
  if (!ret)
    return ret.error();

  val.change = std::move(tmp);
  return {};
}

HogDBStatus<void>
BlockTX::commit()
{
  std::mutex lock;
  HogDBStatus<void> ret;

  auto commit_fn = [this, &lock,
                    &ret](std::pair<const ItemKey, const ItemLock &> v) {
    HogDBStatus<void> tmp = v.second.change.commit(db_, v.first);
    if (!tmp) [[unlikely]] {
      std::scoped_lock guard(lock);
      ret = tmp;
    }
  };
  locks_.for_each_parallel(commit_fn);
  return ret;
}

} // namespace hogd
