#pragma once

#include <memory>

#include "hogd/base/types.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

template<item_lock_unsized T> class PointerLock : public std::unique_ptr<T> {
public:
  using change_type = T::change_type;

  template<typename... Args>
  PointerLock(Args &&...args)
    : std::unique_ptr<T>(std::make_unique<T>(std::forward<Args>(args)...))
  {}

  HogDBStatus<int128_t> try_atomic_change_lock(const change_type *before,
                                               const change_type *after,
                                               const LockContext &ctx)
  {
    return this->get()->try_atomic_change_lock(before, after, ctx);
  }
  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         change_type &after, const LockContext &ctx)
  {
    return this->get()->commit_change(did_finalize, snapshot_cost, after, ctx);
  }
  HogDBStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const
  {
    return this->get()->stage_committed_changes(ctx, changes);
  }
};

} // namespace hogd
