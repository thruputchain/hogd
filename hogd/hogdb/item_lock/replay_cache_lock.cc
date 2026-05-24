#include "hogd/hogdb/item_lock/replay_cache_lock.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <iterator>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/testlock.h"
#include "x23/vec.h"

namespace hogd {

HogDBStatus<ReplayCacheMetadata *>
ReplayCacheLock::get_meta(const LockContext *ctx) const
{
  ReplayCacheMetadata *r = meta_.load(std::memory_order_acquire);
  if (r != nullptr) {
    return {r};
  }

  if (!ctx) {
    return HogDBError{HogDBCode::kInternal};
  }

  HogDBStatus<void> ret;
  std::call_once(meta_once_flag_, [this, &ctx, &ret]() {
    assert(ctx->dao);
    auto meta =
        ctx->dao->read_address_replay_cache_meta(ctx->key.get_address());
    if (!meta) {
      if (meta.error() == HogDBCode::kNotFound) {
        meta_cell_.emplace(ReplayCacheMetadata{});
      }
      else {
        meta_poisoned_ = true;
        ret = meta.error();
        return;
      }
    }
    else {
      meta_cell_.emplace(std::move(*meta));
    }
    meta_.store(&*meta_cell_, std::memory_order_release);
    ret = {};
  });

  // First caller gets the specific error from ret.
  if (!ret.has_value()) {
    return ret.error();
  }

  // Subsequent callers: call_once provides happens-before, so
  // meta_poisoned_ is safely visible without an atomic.
  if (meta_poisoned_) {
    return HogDBError{HogDBCode::kInternal};
  }

  while ((r = meta_.load(std::memory_order_acquire)) == nullptr) {
    cpu_relax();
  }

  return {r};
}

HogDBStatus<ReplayCacheData>
ReplayCacheLock::build_result(const LockContext &ctx, BlockHeight block_height,
                              const std::span<Uniquifier> &uniquifiers) const
{
  CHECK(block_height >= ctx.block_height);

  auto r = get_base(ctx, block_height);
  if (!r) {
    return r.error();
  }
  auto &base = *r.value();

  // Get an iterator to the range of additions with matching
  // block_expiry_height. Caller must sort uniquifiers so the
  // block_expiry_height projection is non-decreasing; Uniquifier's
  // default order (block_expiry_height, then nonce) satisfies this.
  assert(std::ranges::is_sorted(uniquifiers, {},
                                &Uniquifier::block_expiry_height));
  auto [b, e] = std::ranges::equal_range(uniquifiers, block_height, {},
                                         &Uniquifier::block_expiry_height);
  auto filtered_nonces = std::ranges::subrange(b, e) |
                         std::views::transform([](const auto &uniquifier) {
                           return uniquifier.nonce;
                         });

  ReplayCacheData result;
  // An extra iteration over filtered_nonces avoids extra allocs.
  result.nonces.reserve(base.data.nonces.size() + std::distance(b, e));
  // Merge sorted ranges to produce a sorted base nonce list.
  std::ranges::set_union(base.data.nonces, filtered_nonces,
                         std::back_inserter(result.nonces));

  return result;
}

HogDBStatus<ReplayCacheLock::ActiveHeight *>
ReplayCacheLock::get_base(const LockContext &ctx,
                          BlockHeight block_height) const
{
  {
    // Fast path; shared lock only.
    auto sl = bases_lock_.hold_shared();
    auto it = bases_.find(block_height);
    if (it != bases_.end()) {
      return it->second.get();
    }
  }

  std::unique_lock<x23::TestLock> ul(bases_lock_);

  // Seems dumb to double check, but x23::SharedLock doesn't support upgrades,
  // and I'd rather not have wasted accesses to the db.
  auto it = bases_.find(block_height);
  if (it != bases_.end()) {
    return it->second.get();
  }

  auto base = ctx.dao->read_address_replay_cache_data(ctx.key.get_address(),
                                                      block_height);

  std::unique_ptr<ActiveHeight> entry{nullptr};
  if (!base) {
    if (base.error() != HogDBCode::kNotFound) {
      return base.error();
    }
    // New height. Check global active height limit.
    auto meta = get_meta(&ctx);
    if (!meta) {
      return meta.error();
    }
    auto pending = pending_new_heights_.fetch_add(1, std::memory_order_relaxed);
    if (meta.value()->heights.size() + pending + 1 >
        kReplayCacheHeightsMaxLen) {
      pending_new_heights_.fetch_sub(1, std::memory_order_relaxed);
      return HogDBError{HogDBCode::kConflict};
    }
    entry = std::make_unique<ActiveHeight>();
  }
  else {
    entry = std::make_unique<ActiveHeight>(std::move(*base));
  }

  auto res = bases_.emplace(block_height, std::move(entry));
  return res.first->second.get();
}

namespace {

class UniquifierHelper {
public:
  static Uniquifier from_uint128(uint128_t v)
  {
    return Uniquifier(v >> 64, v & 0xFFFFFFFFFFFFFFFF);
  }
  static uint128_t to_uint128(const Uniquifier &u)
  {
    return (static_cast<uint128_t>(u.block_expiry_height) << 64) | u.nonce;
  }
};

uint64_t
count_pruned_replay_cache_entries(const ReplayCacheMetadata &meta,
                                  BlockHeight block_height)
{
  auto prune_end = std::ranges::lower_bound(meta.heights, block_height, {},
                                            &ReplayCacheHeightCount::height);

  uint64_t pruned_count = 0;
  for (auto it = meta.heights.begin(); it != prune_end; ++it) {
    pruned_count += it->nonce_count;
  }
  return pruned_count;
}

} // namespace

HogDBStatus<int128_t>
ReplayCacheLock::try_atomic_change_lock(
    const PointerItem<ReplayCacheIt> *before,
    const PointerItem<ReplayCacheIt> *after, const LockContext &ctx)
{
  // No single transaction is allowed to add to the replay cache twice.
  if (before != nullptr && after != nullptr) {
    return HogDBError{HogDBCode::kConflict};
  }

  // Need to remove a previously added Uniquifier (e.g. if a tx aborts).
  if (after == nullptr) {
    auto u = before->get()->addition();
    auto r = get_base(ctx, u.block_expiry_height);
    if (!r.has_value()) {
      return HogDBError{HogDBCode::kInternal};
    }
    auto &base = *r.value();
    conflicts_.erase(UniquifierHelper::to_uint128(u));
    base.pending_additions.fetch_sub(1, std::memory_order_relaxed);
    return {};
  }

  const auto &addition = after->get()->addition();

  // Check expiry
  if (addition.block_expiry_height < ctx.block_height) {
    trace("attempted to consume {}.{} at height {}",
          addition.block_expiry_height, addition.nonce, ctx.block_height);
    return HogDBError{HogDBCode::kConflict};
  }

  if (conflicts_.contains(UniquifierHelper::to_uint128(addition))) {
    return HogDBError{HogDBCode::kConflict};
  }

  // Check if this nonce already exists in the base data for this height.
  auto r = get_base(ctx, addition.block_expiry_height);
  if (!r) {
    return r.error();
  }
  auto &base = *r.value();

  if (std::ranges::binary_search(base.data.nonces.begin(),
                                 base.data.nonces.end(), addition.nonce)) {
    trace("attempted to consume {}.{} but it was already the base cache",
          addition.block_expiry_height, addition.nonce);
    return HogDBError{HogDBCode::kConflict};
  }

  // Reserve slot
  auto additions =
      base.pending_additions.fetch_add(1, std::memory_order_relaxed);

  // Check limit
  if (base.data.nonces.size() + additions + 1 > kReplayCacheNoncesMaxLen) {
    base.pending_additions.fetch_sub(1, std::memory_order_relaxed);
    return HogDBError{HogDBCode::kConflict};
  }

  if (!conflicts_.try_insert(UniquifierHelper::to_uint128(addition))) {
    base.pending_additions.fetch_sub(1, std::memory_order_relaxed);
    return HogDBError{HogDBCode::kConflict};
  }

  auto meta = get_meta(&ctx);
  if (!meta) {
    return meta.error();
  }

  // Reservation stays snapshot-based. GC credit is only applied once this
  // change commits, so multiple txs cannot spend the same future refund.
  return ctx.cost_model->get_replay_cache_cost(meta.value()->size + 1);
}

int128_t
ReplayCacheLock::commit_change(bool /*did_finalize*/, int128_t snapshot_cost,
                               const PointerItem<ReplayCacheIt> & /*after*/,
                               const LockContext &ctx)
{
  assert(meta_.load(std::memory_order_acquire) != nullptr);
  auto &meta = **get_meta(nullptr);

  std::call_once(pruned_count_once_flag_, [this, &meta, &ctx]() {
    uint64_t pruned = count_pruned_replay_cache_entries(meta, ctx.block_height);
    CHECK_LE(pruned, static_cast<uint64_t>(meta.size));
    cached_pruned_count_.store(pruned, std::memory_order_relaxed);
    pruned_count_block_height_.store(ctx.block_height,
                                     std::memory_order_relaxed);
  });
  assert(pruned_count_block_height_.load(std::memory_order_relaxed) ==
         ctx.block_height);
  uint64_t pruned_count = cached_pruned_count_.load(std::memory_order_relaxed);

  uint64_t live_size_after_gc = static_cast<uint64_t>(meta.size) - pruned_count;
  uint64_t committed_additions_before =
      committed_additions_.fetch_add(1, std::memory_order_relaxed);

  uint64_t committed_size_before =
      live_size_after_gc + committed_additions_before;
  uint64_t committed_size_after = committed_size_before + 1;
  CHECK_GT(committed_size_after, 0U);

  // Reservations and commits are not the same thing: later in-block additions
  // may already be reserved in conflicts_ but still abort. The committed
  // counter, not ItemLock finalization state, defines the first committed
  // replay-cache addition that may realize the snapshot->post-GC transition.
  // Later commits charge incremental growth from the already-committed size.
  int128_t previous_cost =
      committed_additions_before == 0
          ? snapshot_cost
          : ctx.cost_model->get_replay_cache_cost(committed_size_before);
  int128_t new_cost =
      ctx.cost_model->get_replay_cache_cost(committed_size_after);

  return new_cost - previous_cost;
}

HogDBStatus<void>
ReplayCacheLock::stage_committed_changes(const LockContext &ctx,
                                         ChangeSet &changes) const
{
  CHECK(ctx.key.is_replay_cache());

  CHECK(meta_.load(std::memory_order_acquire) != nullptr);
  auto meta = **get_meta(nullptr);

  auto sl = bases_lock_.hold_shared();

  x23::Vec<ReplayCacheHeightCount> new_heights;
  new_heights.reserve(meta.heights.size() + bases_.size());

  auto prune_end = std::ranges::lower_bound(meta.heights, ctx.block_height, {},
                                            &ReplayCacheHeightCount::height);

  // Prune old heights.
  size_t pruned_count = 0;
  for (auto it = meta.heights.begin(); it != prune_end; ++it) {
    auto key = make_replay_cache_data_key(ctx.key.get_address(), it->height);
    changes.remove(key);
    pruned_count += it->nonce_count;
  }

  auto it = prune_end;
  auto end = meta.heights.end();

  std::vector<Uniquifier> all_additions;
  all_additions.reserve(conflicts_.size());
  for (const uint128_t &value : conflicts_) {
    all_additions.push_back(UniquifierHelper::from_uint128(value));
  }
  std::ranges::sort(all_additions);

  for (BlockHeight height : std::views::keys(bases_)) {
    CHECK(height >= ctx.block_height);

    while (it != end && it->height < height) {
      new_heights.push_back(*it);
      ++it;
    }
    if (it != end && it->height == height) {
      ++it;
    }

    auto res = build_result(ctx, height, all_additions);
    if (!res) {
      return res.error();
    }

    // A fresh (is_new) height whose only reservation was aborted leaves an
    // empty base here. Skip serializing so we don't persist a zero-count
    // meta entry or an empty data row.
    if (res->nonces.empty()) {
      continue;
    }

    auto key = make_replay_cache_data_key(ctx.key.get_address(), height);
    auto buf = x23::serialize(*res);
    changes.put(key, 0, buf);

    new_heights.emplace_back(height,
                             static_cast<unsigned int>(res->nonces.size()));
  }

  while (it != end) {
    new_heights.push_back(*it);
    ++it;
  }

  meta.size = meta.size - pruned_count + all_additions.size();
  meta.heights = std::move(new_heights);

  auto meta_key = make_address_key(ctx.key.get_address(), Prefix::kReplayCache);
  auto meta_buf = x23::serialize(meta);
  changes.put(meta_key, 0, meta_buf);

  return {};
}

} // namespace hogd
