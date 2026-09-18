#pragma once

#include "hogd/base/types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

class DeletionLock {
public:
  using change_type = Deletion;

  DeletionLock() = default;

  // Admits a deletion. Bounds the block-close cleanup fan-out of a
  // retrocausal-queue delete (see the .cc for details); other deletions and
  // lock releases (`after == nullptr`) are admitted unconditionally.
  static HogDbStatus<int128_t> try_atomic_change_lock(const Deletion *before,
                                                      const Deletion *after,
                                                      const LockContext &ctx);

  static int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                                Deletion & /*unused*/,
                                const LockContext & /*ctx*/)
  {
    if (did_finalize) {
      return -snapshot_cost;
    }
    return 0;
  }

  static HogDbStatus<void> stage_committed_changes(const LockContext &ctx,
                                                   ChangeSet &changes);
};

static_assert(item_lock<DeletionLock>);

} // namespace hogd
