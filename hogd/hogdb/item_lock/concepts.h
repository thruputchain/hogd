#pragma once

#include <concepts>

#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "types.hh"

namespace hogd {

// Passed to item_lock::try_atomic_change_lock and item_lock::commit_change.
// Used by some lock types to access the DAO, block height, and/or item key.
// e.g. ReplayCacheLock uses this is access the block height to determine if
// a uniquifier is expired and to lazily load chunks of replay caches.
struct LockContext {
  LockContext(HogDBDAO *dao, BlockHeight block_height, const ItemKey &key,
              const StorageCostModel *cost_model)
    : dao(dao), block_height(block_height), key(key), cost_model(cost_model)
  {}

  HogDBDAO *dao;
  BlockHeight block_height;
  const ItemKey &key;
  const StorageCostModel *cost_model;
};

template<typename Lock>
concept item_lock_unsized =
    requires { typename Lock::change_type; } &&
    requires(Lock lock, const Lock::change_type *before,
             const Lock::change_type *after, const LockContext &ctx,
             bool finalize, int128_t snapshot_cost) {
      {
        lock.try_atomic_change_lock(before, after, ctx)
      } -> std::same_as<HogDBStatus<int128_t>>;
      {
        lock.commit_change(finalize, snapshot_cost, *after, ctx.cost_model)
      } -> std::same_as<int128_t>;
    } &&
    requires(Lock const &lock, const LockContext &ctx) {
      {
        lock.write_committed_change(ctx)
      } -> std::same_as<HogDBStatus<void>>;
    };

template<typename Lock>
concept item_lock = item_lock_unsized<Lock> && requires { sizeof(Lock) <= 64; };
} // namespace hogd
