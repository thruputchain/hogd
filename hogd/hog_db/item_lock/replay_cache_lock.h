#pragma once

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/container/concurrent_set.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/item_lock/pointer_lock.h"
#include "hogd/hog_db/status.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/testlock.h"

namespace hogd {

// Hash traits for the replay-cache conflict set. Runs the full 128-bit
// (expiry_height, nonce) key through the process-wide keyed SipHash24 rather
// than std::hash<uint128_t>, which is implementation-defined and on libstdc++
// hashes only the low (nonce) limb, so distinct-expiry same-nonce entries all
// collapse into one bucket. (#2663)
struct ReplayConflictSetTraits {
  static constexpr uint128_t kEmpty = uint128_t{};
  static constexpr uint128_t kTombstone = ~uint128_t{};
  static constexpr uint128_t kInserting = ~uint128_t{} - 1;
  static size_t hash(const uint128_t &v);
};

class ReplayCacheLock {
public:
  using change_type = PointerItem<ReplayCacheIt>;

  HogDbStatus<void> initialize(LockContext const &ctx);

  HogDbStatus<int128_t>
  try_atomic_change_lock(const PointerItem<ReplayCacheIt> *before,
                         const PointerItem<ReplayCacheIt> *after,
                         const LockContext &ctx);
  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         PointerItem<ReplayCacheIt> &after,
                         const LockContext &ctx);
  HogDbStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

private:
  HogDbStatus<ReplayCacheData>
  build_result(const LockContext &ctx, BlockHeight block_height,
               const std::span<Uniquifier> &uniquifiers) const;

  struct ActiveHeight {
    ActiveHeight(ReplayCacheData &&data) : data(std::move(data)) {}
    ActiveHeight() : data(), is_new(true) {}
    ReplayCacheData data;
    std::atomic<int32_t> pending_additions{0};
    bool is_new{false};
  };

  HogDbStatus<ActiveHeight *> get_base(const LockContext &ctx,
                                       BlockHeight block_height) const;
  HogDbStatus<ReplayCacheMetadata *> get_meta(const LockContext *ctx) const;

  // Verifies that room remains for one more fresh height and, if so, bumps
  // pending_new_heights_ accordingly. Returns kDbConflictCapacityExceeded if
  // the cap would be exceeded (rolling the bump back).
  HogDbStatus<void> charge_pending_height(const LockContext &ctx) const;
  // Decrement pending_additions on a released reservation and, when a fresh
  // height's last outstanding reservation goes away (its 1->0 transition),
  // give the pending_new_heights_ budget back.
  void release_pending_addition(ActiveHeight &base) const;

  mutable std::map<BlockHeight, ActiveHeight> bases_;
  mutable x23::SharedLock bases_lock_;

  mutable std::atomic<ReplayCacheMetadata *> meta_;
  mutable std::optional<ReplayCacheMetadata> meta_cell_;
  mutable std::once_flag meta_once_flag_;
  mutable bool meta_poisoned_{false};

  mutable ConcurrentSet<uint128_t, ReplayConflictSetTraits> conflicts_;
  mutable std::atomic<uint64_t> committed_additions_{0};

  mutable std::atomic<int32_t> pending_new_heights_{0};

  // Cached result of count_pruned_replay_cache_entries, computed once per block
  // on the first commit_change call. The value depends only on the snapshot
  // metadata and ctx.block_height, both of which are constant within a block.
  mutable std::atomic<uint64_t> cached_pruned_count_{0};
  mutable std::once_flag pruned_count_once_flag_;
  mutable std::atomic<BlockHeight> pruned_count_block_height_{0};

  friend class ReplayCacheIt;
  friend class ReplayCacheLockTestHelper;
};

static_assert(item_lock_unsized<ReplayCacheLock>);
static_assert(item_lock<PointerLock<ReplayCacheLock>>);

} // namespace hogd
