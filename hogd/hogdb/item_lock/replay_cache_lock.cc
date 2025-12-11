#include "hogd/hogdb/item_lock/replay_cache_lock.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <mutex>
#include <ranges>
#include <span>

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

namespace hogd {

namespace {
bool
uniquifier_less(const Uniquifier &a, const Uniquifier &b)
{
  if (a.block_expiry_height != b.block_expiry_height) {
    return a.block_expiry_height < b.block_expiry_height;
  }
  return a.nonce < b.nonce;
}
} // namespace

ReplayCacheData
ReplayCacheLock::build_result(BlockHeight block_height) const
{
  ReplayCacheData result;
  if (!base_replay_cache_.has_value()) {
    // Should be loaded by now if we are committing
    return result;
  }

  result.uniquifiers.reserve(base_replay_cache_->uniquifiers.size() +
                             additions_.size());

  // Filter out expired entries.
  auto pruned_uniquifiers =
      base_replay_cache_->uniquifiers | std::views::filter([&](const auto &u) {
        return u.block_expiry_height >= block_height;
      });

  // Merge in linear time
  std::ranges::set_union(pruned_uniquifiers, additions_,
                         std::back_inserter(result.uniquifiers));

  return result;
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

  if (!base_replay_cache_.has_value()) {
    auto res = ctx.dao.read_address_replay_cache(ctx.key.get_address());
    if (!res) {
      if (res.error() == HogDBCode::kNotFound) {
        base_replay_cache_ = ReplayCacheData{};
      }
      else {
        return res.error();
      }
    }
    else {
      base_replay_cache_ = *res;
    }
  }

  // Need to remove a previously added Uniquifier (e.g. if a tx aborts).
  if (after == nullptr) {
    CHECK(additions_.contains(before->get()->addition()));
    additions_.erase(before->get()->addition());
    return {};
  }

  const auto &addition = after->get()->addition();

  // Check expiry
  if (addition.block_expiry_height < ctx.block_height) {
    return HogDBError{HogDBCode::kConflict};
  }

  if (additions_.contains(addition)) {
    return HogDBError{HogDBCode::kConflict};
  }

  if (std::binary_search(base_replay_cache_->uniquifiers.begin(),
                         base_replay_cache_->uniquifiers.end(), addition,
                         uniquifier_less)) {
    return HogDBError{HogDBCode::kConflict};
  }

  additions_.insert(addition);

  return cost_model->get_replay_cache_cost(
      base_replay_cache_->uniquifiers.size() + 1);
}

int128_t
ReplayCacheLock::commit_change(bool did_finalize, int128_t snapshot_cost,
                               const PointerItem<ReplayCacheIt> & /*after*/,
                               const StorageCostModel *cost_model)
{
  std::unique_lock<x23::TestLock> ul(lock_);

  if (!base_replay_cache_.has_value()) {
    panic("ReplayCacheLock::commit_change called without a base replay cache");
  }

  int128_t previous_cost = did_finalize
                               ? snapshot_cost
                               : cost_model->get_replay_cache_cost(
                                     base_replay_cache_->uniquifiers.size());

  int128_t new_cost = cost_model->get_replay_cache_cost(
      base_replay_cache_->uniquifiers.size() + 1);

  return new_cost - previous_cost;
}

HogDBStatus<void>
ReplayCacheLock::write_committed_change(LockContext const &ctx,
                                        MerkleTriePrefix &trie) const
{
  assert(ctx.key.is_replay_cache());

  auto block_height = ctx.block_height;
  auto res = build_result(block_height);

  auto ret = ctx.dao.write_address_replay_cache(ctx.key.get_address(), res);
  if (!ret) {
    return ret.error();
  }

  auto buf = x23::serialize(res);
  CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(trie, ctx.key, 0,
                                                  std::as_bytes(std::span(buf)))
            .has_value());
  return ret;
}

} // namespace hogd
