#include "hogd/hogdb/item_lock/replay_cache_lock.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <mutex>
#include <ranges>
#include <span>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/testlock.h"
#include "x23/vec.h"

namespace hogd {

HogDBStatus<ReplayCacheData>
ReplayCacheLock::build_result(const LockContext &ctx,
                              BlockHeight block_height) const
{
  CHECK(block_height >= ctx.block_height);

  auto base = get_base(ctx, block_height);
  if (!base) {
    return base.error();
  }

  // Get an iterator to the range of additions with matching
  // block_expiry_height.
  Uniquifier l(block_height, 0);
  Uniquifier u(block_height + 1, 0);
  auto b = additions_.lower_bound(l);
  auto e = additions_.lower_bound(u);
  auto filtered_nonces = std::ranges::subrange(b, e) |
                         std::views::transform([](const auto &uniquifier) {
                           return uniquifier.nonce;
                         });

  ReplayCacheData result;
  // An extra iteration over filtered_nonces avoids extra allocs.
  result.nonces.reserve(base.value()->nonces.size() + std::distance(b, e));
  // Merge sorted ranges to produce a sorted base nonce list.
  std::ranges::set_union(base.value()->nonces, filtered_nonces,
                         std::back_inserter(result.nonces));

  return result;
}

HogDBStatus<ReplayCacheData *>
ReplayCacheLock::get_base(const LockContext &ctx,
                          BlockHeight block_height) const
{
  if (bases_.contains(block_height)) {
    return &bases_[block_height];
  }

  auto base = ctx.dao->read_address_replay_cache_data(ctx.key.get_address(),
                                                      block_height);
  if (!base) {
    if (base.error() != HogDBCode::kNotFound) {
      return base.error();
    }
    bases_.emplace(block_height, ReplayCacheData{});
  }
  else {
    bases_.emplace(block_height, *base);
  }
  return &bases_[block_height];
}

HogDBStatus<int128_t>
ReplayCacheLock::try_atomic_change_lock(
    const PointerItem<ReplayCacheIt> *before,
    const PointerItem<ReplayCacheIt> *after, const StorageCostModel *cost_model,
    const LockContext &ctx)
{
  // No single transaction is allowed to add to the replay cache twice.
  if (before != nullptr && after != nullptr) {
    return HogDBError{HogDBCode::kConflict};
  }

  std::unique_lock<x23::TestLock> ul(lock_);

  // Need to remove a previously added Uniquifier (e.g. if a tx aborts).
  if (after == nullptr) {
    CHECK(additions_.contains(before->get()->addition()));
    additions_.erase(before->get()->addition());
    return {};
  }

  const auto &addition = after->get()->addition();

  // Check expiry
  if (addition.block_expiry_height < ctx.block_height) {
    trace("attempted to consume {}.{} at height {}",
          addition.block_expiry_height, addition.nonce, ctx.block_height);
    return HogDBError{HogDBCode::kConflict};
  }

  if (additions_.contains(addition)) {
    trace("attempted to consume {}.{} but it was already in cache additions",
          addition.block_expiry_height, addition.nonce);
    return HogDBError{HogDBCode::kConflict};
  }

  // Check if this nonce already exists in the base data for this height.
  auto base = get_base(ctx, addition.block_expiry_height);
  if (!base) {
    return base.error();
  }
  if (std::binary_search(base.value()->nonces.begin(),
                         base.value()->nonces.end(), addition.nonce)) {
    trace("attempted to consume {}.{} but it was already the base cache",
          addition.block_expiry_height, addition.nonce);
    return HogDBError{HogDBCode::kConflict};
  }

  additions_.insert(addition);

  if (!meta_.has_value()) {
    assert(ctx.dao);
    auto meta = ctx.dao->read_address_replay_cache_meta(ctx.key.get_address());
    if (!meta) {
      if (meta.error() == HogDBCode::kNotFound) {
        meta_.emplace(ReplayCacheMetadata{});
      }
      else {
        return meta.error();
      }
    }
    else {
      meta_.emplace(std::move(*meta));
    }
  }

  return cost_model->get_replay_cache_cost(meta_->size + 1);
}

int128_t
ReplayCacheLock::commit_change(bool did_finalize, int128_t snapshot_cost,
                               const PointerItem<ReplayCacheIt> & /*after*/,
                               const StorageCostModel *cost_model)
{
  std::unique_lock<x23::TestLock> ul(lock_);

  if (!meta_.has_value()) {
    panic("ReplayCacheLock::commit_change called without meta populated");
  }

  int128_t previous_cost = did_finalize
                               ? snapshot_cost
                               : cost_model->get_replay_cache_cost(meta_->size);

  int128_t new_cost = cost_model->get_replay_cache_cost(meta_->size + 1);

  return new_cost - previous_cost;
}

HogDBStatus<void>
ReplayCacheLock::write_committed_change(LockContext const &ctx,
                                        MerkleTriePrefix &trie) const
{
  assert(ctx.key.is_replay_cache());

  if (!meta_.has_value()) {
    panic("write_committed_change called without meta populated");
  }

  // Build new heights list while iterating through list of heights modified in
  // this block.
  x23::Vec<ReplayCacheHeightCount> new_heights;
  new_heights.reserve(meta_->heights.size() + bases_.size());

  // Find heights to prune: all heights < ctx.block_height (strictly less than)
  // Heights == ctx.block_height should be kept as they are still valid.
  auto prune_end =
      std::ranges::lower_bound(meta_->heights, ctx.block_height, {},
                               &ReplayCacheHeightCount::height);

  // Delete old heights from DAO. Track total pruned nonces.
  size_t pruned_count = 0;
  for (auto it = meta_->heights.begin(); it != prune_end; ++it) {
    trace("pruning height {}, deleteing {} nonces from address {}", it->height,
          it->nonce_count, ctx.key.get_address());
    assert(ctx.dao);
    auto ret = ctx.dao->delete_address_replay_cache_data(ctx.key.get_address(),
                                                         it->height);
    if (!ret) {
      return ret.error();
    }
    pruned_count += it->nonce_count;

    auto key = make_replay_cache_data_key(ctx.key.get_address(), it->height);
    auto trie_key_bytes =
        MerkleTrieHelpers::add_type_to_merkle_key(std::span(key).subspan(1), 0);
    CHECK(trie.remove(trie_key_bytes));
  }

  auto it = prune_end;
  auto end = meta_->heights.end();

  // Track through all heights with changes.
  for (BlockHeight height : std::views::keys(bases_)) {
    CHECK(height >= ctx.block_height);
    // Copy any heights from meta_ that come before this one
    while (it != end && it->height < height) {
      new_heights.push_back(*it);
      ++it;
    }

    // Skip the old entry for this height if it exists (we're replacing it)
    if (it != end && it->height == height) {
      ++it;
    }

    auto res = build_result(ctx, height);
    if (!res) {
      return res.error();
    }

    auto ret = ctx.dao->write_address_replay_cache_data(ctx.key.get_address(),
                                                        height, *res);
    if (!ret) {
      return ret.error();
    }

    auto buf = x23::serialize(*res);
    auto key = make_replay_cache_data_key(ctx.key.get_address(), height);
    auto trie_key_bytes =
        MerkleTrieHelpers::add_type_to_merkle_key(std::span(key).subspan(1), 0);
    CHECK(trie.insert_or_update(trie_key_bytes, std::as_bytes(std::span(buf))));

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

  meta_->size = meta_->size - pruned_count + additions_.size();
  meta_->heights = std::move(new_heights);
  trace("writing meta for address {} new size {} active height count {}",
        ctx.key.get_address(), meta_->size, meta_->heights.size());
  auto ret = ctx.dao->write_address_replay_cache_meta(ctx.key.get_address(),
                                                      meta_.value());
  if (!ret) {
    return ret.error();
  }

  auto buf = x23::serialize(meta_.value());
  CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(trie, ctx.key, 0,
                                                  std::as_bytes(std::span(buf)))
            .has_value());

  return {};
}

} // namespace hogd
