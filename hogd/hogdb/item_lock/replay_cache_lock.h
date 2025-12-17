#pragma once

#include <optional>
#include <set>

#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_item.hh"
#include "x23/testlock.h"

namespace hogd {

class ReplayCacheLock {
public:
  using change_type = PointerItem<ReplayCacheIt>;

  HogDBStatus<int128_t>
  try_atomic_change_lock(const PointerItem<ReplayCacheIt> *before,
                         const PointerItem<ReplayCacheIt> *after,
                         const StorageCostModel *cost_model,
                         const LockContext &ctx);
  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         const PointerItem<ReplayCacheIt> &after,
                         const StorageCostModel *cost_model);
  HogDBStatus<void> write_committed_change(LockContext const &ctx,
                                           MerkleTriePrefix &trie) const;

private:
  HogDBStatus<ReplayCacheData> build_result(const LockContext &ctx,
                                            BlockHeight block_height) const;
  HogDBStatus<ReplayCacheData *> get_base(const LockContext &ctx,
                                          BlockHeight block_height) const;

  mutable std::map<BlockHeight, ReplayCacheData> bases_;
  mutable std::optional<ReplayCacheMetadata> meta_;
  std::set<Uniquifier> additions_;

  x23::TestLock lock_;

  friend class ReplayCacheIt;
  friend class ReplayCacheLockTestHelper;
};

static_assert(item_lock_unsized<ReplayCacheLock>);
static_assert(item_lock<PointerLock<ReplayCacheLock>>);

} // namespace hogd
