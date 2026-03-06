#pragma once

#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

class DeletionLock {
public:
  using change_type = Deletion;

  DeletionLock() = default;

  static HogDBStatus<int128_t>
  try_atomic_change_lock(const Deletion * /*unused*/,
                         const Deletion * /*unused*/,
                         const LockContext & /*ctx*/)
  {
    return {0};
  }

  static int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                                const Deletion & /*unused*/,
                                const StorageCostModel * /*unused*/)
  {
    if (did_finalize) {
      return -snapshot_cost;
    }
    return 0;
  }

  static HogDBStatus<void> write_committed_change(LockContext const &ctx);
  static HogDBStatus<void> apply_db_hash_delta(const LockContext &ctx,
                                                  LtHash16 &hash);
};

static_assert(item_lock<DeletionLock>);

} // namespace hogd
