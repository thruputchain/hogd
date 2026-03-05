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

  if (!ret.has_value()) {
    return ret.error();
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
  // block_expiry_height.
  Uniquifier l(block_height, 0);
  Uniquifier u(block_height + 1, 0);
  auto b = std::ranges::lower_bound(uniquifiers, l);
  auto e = std::ranges::lower_bound(uniquifiers, u);
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
    if (meta.value()->heights.size() + pending > kReplayCacheHeightsMaxLen) {
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

  return ctx.cost_model->get_replay_cache_cost(meta.value()->size + 1);
}

int128_t
ReplayCacheLock::commit_change(bool did_finalize, int128_t snapshot_cost,
                               const PointerItem<ReplayCacheIt> & /*after*/,
                               const StorageCostModel *cost_model)
{
  assert(meta_.load(std::memory_order_acquire) != nullptr);
  auto &meta = **get_meta(nullptr);

  int128_t previous_cost = did_finalize
                               ? snapshot_cost
                               : cost_model->get_replay_cache_cost(meta.size);

  int128_t new_cost = cost_model->get_replay_cache_cost(meta.size + 1);

  return new_cost - previous_cost;
}

HogDBStatus<void>
ReplayCacheLock::write_committed_change(LockContext const &ctx) const
{
  assert(ctx.key.is_replay_cache());

  assert(meta_.load(std::memory_order_acquire) != nullptr);
  auto meta = **get_meta(nullptr);

  // Other operations on the lock should be quiescent, but acquiring the read
  // lock here ensures we acquire any lazy init of elements in bases_.
  auto sl = bases_lock_.hold_shared();

  // Build new heights list while iterating through list of heights modified
  // in this block.
  x23::Vec<ReplayCacheHeightCount> new_heights;
  new_heights.reserve(meta.heights.size() + bases_.size());

  // Find heights to prune: all heights < ctx.block_height (strictly less
  // than) Heights == ctx.block_height should be kept as they are still valid.
  auto prune_end = std::ranges::lower_bound(meta.heights, ctx.block_height, {},
                                            &ReplayCacheHeightCount::height);

  // Delete old heights from DAO. Track total pruned nonces.
  size_t pruned_count = 0;
  for (auto it = meta.heights.begin(); it != prune_end; ++it) {
    trace("pruning height {}, deleteing {} nonces from address {}", it->height,
          it->nonce_count, ctx.key.get_address());
    assert(ctx.dao);
    auto ret = ctx.dao->remove_address_replay_cache_data(ctx.key.get_address(),
                                                         it->height);
    if (!ret) {
      return ret.error();
    }
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

  // Track through all heights with changes.
  for (BlockHeight height : std::views::keys(bases_)) {
    CHECK(height >= ctx.block_height);
    // Copy any heights from meta that come before this one
    while (it != end && it->height < height) {
      new_heights.push_back(*it);
      ++it;
    }

    // Skip the old entry for this height if it exists (we're replacing it)
    if (it != end && it->height == height) {
      ++it;
    }

    auto res = build_result(ctx, height, all_additions);
    if (!res) {
      return res.error();
    }

    auto ret = ctx.dao->write_address_replay_cache_data(ctx.key.get_address(),
                                                        height, *res);
    if (!ret) {
      return ret.error();
    }

    auto buf = x23::serialize(*res);

    // Slot in replacement height/nonce count.
    // Don't worry about tracking delta to total cache size here.
    // additions_.size() already accounts for this.
    new_heights.emplace_back(height,
                             static_cast<unsigned int>(res->nonces.size()));
  }

  while (it != end) {
    new_heights.push_back(*it);
    ++it;
  }

  meta.size = meta.size - pruned_count + all_additions.size();
  meta.heights = std::move(new_heights);
  trace("writing meta for address {} new size {} active height count {}",
        ctx.key.get_address(), meta.size, meta.heights.size());
  auto ret =
      ctx.dao->write_address_replay_cache_meta(ctx.key.get_address(), meta);
  if (!ret) {
    return ret.error();
  }

  return {};
}

HogDBStatus<void>
ReplayCacheLock::apply_close_hash_delta(const LockContext & /*ctx*/,
                                        LtHash16 & /*hash*/) 
{
  return {};
}

} // namespace hogd
