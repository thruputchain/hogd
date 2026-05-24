#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/container/concurrent_set.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/status.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/testlock.h"

namespace hogd {

class ReplayCacheLock {
public:
  using change_type = PointerItem<ReplayCacheIt>;

  HogDBStatus<void> initialize(LockContext const &ctx);

  HogDBStatus<int128_t>
  try_atomic_change_lock(const PointerItem<ReplayCacheIt> *before,
                         const PointerItem<ReplayCacheIt> *after,
                         const LockContext &ctx);
  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         PointerItem<ReplayCacheIt> &after,
                         const LockContext &ctx);
  HogDBStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

private:
  HogDBStatus<ReplayCacheData>
  build_result(const LockContext &ctx, BlockHeight block_height,
               const std::span<Uniquifier> &uniquifiers) const;

  struct ActiveHeight {
    ActiveHeight(ReplayCacheData &&data) : data(std::move(data)) {}
    ActiveHeight() : data(), is_new(true) {}
    ReplayCacheData data;
    std::atomic<int32_t> pending_additions{0};
    bool is_new{false};
  };

  HogDBStatus<ActiveHeight *> get_base(const LockContext &ctx,
                                       BlockHeight block_height) const;
  HogDBStatus<ReplayCacheMetadata *> get_meta(const LockContext *ctx) const;

  mutable std::map<BlockHeight, std::unique_ptr<ActiveHeight>> bases_;
  mutable x23::SharedLock bases_lock_;

  mutable std::atomic<ReplayCacheMetadata *> meta_;
  mutable std::optional<ReplayCacheMetadata> meta_cell_;
  mutable std::once_flag meta_once_flag_;
  mutable bool meta_poisoned_{false};

  mutable ConcurrentSet<uint128_t> conflicts_;
  mutable std::atomic<uint64_t> committed_additions_{0};

  mutable std::atomic<int32_t> pending_new_heights_{0};

  friend class ReplayCacheIt;
  friend class ReplayCacheLockTestHelper;
};

static_assert(item_lock_unsized<ReplayCacheLock>);
static_assert(item_lock<PointerLock<ReplayCacheLock>>);

} // namespace hogd
