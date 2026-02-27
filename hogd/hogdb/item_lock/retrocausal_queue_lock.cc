#include "hogd/hogdb/item_lock/retrocausal_queue_lock.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <expected>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

// TODO(stutsman): almost identical to ReplayCacheLock::get_meta. Factor out
// similar to Rust.
HogDBStatus<RetrocausalQueueMetadata *>
RetrocausalQueueLock::get_meta(const LockContext *ctx) const
{
  RetrocausalQueueMetadata *meta = meta_.load(std::memory_order_acquire);
  if (meta) {
    return meta;
  }

  std::unique_lock<std::mutex> lock(meta_init_mtx_);
  meta = meta_.load(std::memory_order_acquire);
  if (meta) {
    return meta;
  }

  CHECK(ctx != nullptr);
  auto r = ctx->dao->read_retrocausal_queue_metadata(ctx->key);
  if (!r) {
    if (r.error() == HogDBError{HogDBCode::kNotFound} ||
        r.error() == HogDBError{HogDBCode::kTypeMismatch}) {
      trace("RCQ metadata not found for {}; creating new zero metadata.",
            ctx->key);
      if (r.error() == HogDBError{HogDBCode::kTypeMismatch}) {
        base_was_different_type_ = true;
      }
      meta_cell_ = RetrocausalQueueMetadata{};
      meta = &*meta_cell_;
      meta_.store(meta, std::memory_order_release);
      return meta;
    }
    return r.error();
  }

  meta_cell_ = *r;
  meta = &*meta_cell_;
  meta_.store(meta, std::memory_order_release);
  return meta;
}

HogDBStatus<int128_t>
RetrocausalQueueLock::try_atomic_change_lock(const change_type *before,
                                             const change_type *after,
                                             const LockContext &ctx)
{
  if (!after) {
    // Undo the changes from before.
    CHECK(before != nullptr);
    auto const &delta = before->delta();

    if (delta.consume) {
      consumed_.store(false, std::memory_order_relaxed);
    }

    const size_t entries_to_unreserve = delta.insert.added_entries.size();
    if (entries_to_unreserve > 0) {
      reserved_entry_count_.fetch_sub(entries_to_unreserve,
                                      std::memory_order_relaxed);
    }

    auto r = get_meta(&ctx);
    if (!r.has_value()) {
      return r.error();
    }
    auto &meta = **r;
    return ctx.cost_model->get_retrocausal_queue_cost(meta.live_entries);
  }

  // This is either a new change or a modification to an existing change.
  CHECK(after != nullptr);
  auto const &delta = after->delta();

  // If before && after has the executor guaranteed that before inserts are a
  // prefix of after inserts? If so, we only need to deal with the new inserts.
  CHECK(before == nullptr ||
        std::equal(before->delta().insert.added_entries.begin(),
                   before->delta().insert.added_entries.end(),
                   delta.insert.added_entries.begin()));

  // Executor should guarantee that before inserts are a prefix of after
  // inserts.
  CHECK(before == nullptr || (delta.insert.added_entries.size() >=
                              before->delta().insert.added_entries.size()));

  // Check strict single-consumer constraint
  const bool this_tx_already_consumed =
      (before != nullptr && before->delta().consume != nullptr);
  if (delta.consume) {
    if (consumed_.load(std::memory_order_relaxed) &&
        !this_tx_already_consumed) {
      // We must abort to maintain MPSC semantics. If this tx has not consumed
      // any entries before, then some other transaction set delta.consume.
      // Hence, there would be two consumers for the block, which is a
      // conflict.
      return HogDBError{HogDBCode::kConflict};
    }

    if (!this_tx_already_consumed) {
      bool expected = false;
      if (!consumed_.compare_exchange_strong(expected, true)) {
        return HogDBError{HogDBCode::kConflict};
      }
    }
  }
  else if (this_tx_already_consumed) {
    // Update removed the consume.
    consumed_.store(false, std::memory_order_relaxed);
  }

  auto r = get_meta(&ctx);
  if (!r.has_value()) {
    return r.error();
  }
  CHECK(*r != nullptr);
  auto &meta = **r;

  // Executor should ensure that we never consume from a block that hasn't been
  // inserted to yet.
  CHECK(meta.max_live_block_num >=
        meta.consume_state.max_fully_consumed_block_number);

  // Reserve space in the RCQ block that will be generated. If there are too
  // many inserts in the same block, abort.
  const size_t new_entries_count = delta.insert.added_entries.size();
  const size_t previous_entries_count =
      (before != nullptr) ? before->delta().insert.added_entries.size() : 0;
  CHECK(new_entries_count >= previous_entries_count);
  const size_t additional_entries = new_entries_count - previous_entries_count;

  if (additional_entries > 0) {
    const size_t reserved_before =
        reserved_entry_count_.fetch_add(additional_entries,
                                        std::memory_order_relaxed);

    if (reserved_before + additional_entries >
        kRetrocausalQueueBlockEntriesMaxLen) {
      reserved_entry_count_.fetch_sub(additional_entries,
                                      std::memory_order_relaxed);
      return HogDBError{HogDBCode::kConflict};
    }
  }

  if (delta.consume) {
    // Now that we know this transaction will commit, we need to do one more
    // thing before we compute storage costs. commit_change() has no DAO access,
    // but the number of entries consumed by this delta depends on the size of
    // stored blocks. Compute the number of entries consumed by this delta, here
    // and record it in case this transaction commits.
    size_t consumed_entries = 0;

    auto const &old_consume = meta.consume_state;
    auto const &new_consume = delta.consume->past_block_consume;

    if (new_consume.max_fully_consumed_block_number ==
        old_consume.max_fully_consumed_block_number) {
      consumed_entries += (new_consume.partial_consumed_number -
                           old_consume.partial_consumed_number);
    }
    else {
      // First block (the one we were partially in)
      auto first_block_num = old_consume.max_fully_consumed_block_number + 1;
      auto first_block =
          ctx.dao->read_retrocausal_queue_block(ctx.key, first_block_num);
      CHECK(first_block.has_value());
      consumed_entries +=
          (first_block->entries.size() - old_consume.partial_consumed_number);

      // Intermediate blocks
      for (uint64_t b = first_block_num + 1;
           b <= new_consume.max_fully_consumed_block_number; ++b) {
        auto block = ctx.dao->read_retrocausal_queue_block(ctx.key, b);
        CHECK(block.has_value());
        consumed_entries += block->entries.size();
      }

      // Last block (the one we end up partially in, which is now the new fully
      // consumed point)
      consumed_entries += new_consume.partial_consumed_number;
    }

    after->consumed_past_entries_ = consumed_entries;
  }

  // Compute the absolute cost for the new entry count. Consumes are not
  // accounted for here; transactions that consume won't see the returned
  // storage cost until they commit.
  const size_t new_live_entries =
      meta.live_entries + delta.insert.added_entries.size();

  int128_t absolute_cost =
      ctx.cost_model->get_retrocausal_queue_cost(new_live_entries);

  // Pessimistic reserve: when inserting into a non-empty RCQ, we must reserve
  // enough to cover the case where a concurrent consume empties the queue
  // before we commit. In that case, commit_change would calculate cost relative
  // to the empty state (cost_before=0), meaning we'd pay the full
  // kBaseItemCost.
  //
  // To ensure cost_change <= locked_storage_cost in all cases:
  //   ItemLock computes: marginal_reserve = absolute_cost - snapshot_cost
  //   We need: marginal_reserve >= cost_if_empty (worst case after concurrent
  //   consume) Therefore: absolute_cost >= snapshot_cost + cost_if_empty
  if (!delta.insert.added_entries.empty() && meta.live_entries > 0) {
    // Compute snapshot cost (cost at block start)
    int128_t snapshot_cost =
        ctx.cost_model->get_retrocausal_queue_cost(meta.live_entries);

    // Cost if the RCQ were empty and we're adding only our entries
    int128_t cost_if_empty = ctx.cost_model->get_retrocausal_queue_cost(
        delta.insert.added_entries.size());

    // Ensure absolute_cost is at least snapshot + cost_if_empty
    absolute_cost = std::max(absolute_cost, snapshot_cost + cost_if_empty);
  }

  return absolute_cost;
}

int128_t
RetrocausalQueueLock::commit_change(bool /*did_finalize*/,
                                    int128_t snapshot_cost,
                                    const change_type &change,
                                    const StorageCostModel *cost_model)
{
  std::unique_lock<std::mutex> lock(mtx_);

  auto r = get_meta(nullptr);
  CHECK(r.has_value());
  auto &meta = **r;

  auto calculate_cost = [&]() {
    int64_t live_entries = meta.live_entries + committed_element_count_delta_;
    // In validation mode, a consumer may commit before its corresponding
    // inserter, temporarily making the count negative.  Clamp to 0 for
    // the cost calculation.  This is safe w.r.t. the CHECK_LE(cost_change,
    // locked_storage_cost) invariant in InvokeTX::commit because clamping
    // from negative to 0 can only make cost_before LARGER, which makes
    // (cost_after - cost_before) SMALLER — tightening the gap below the
    // reservation, not exceeding it.  The pessimistic reserve in
    // try_atomic_change_lock already accounts for the worst case where a
    // concurrent consume empties the queue before the inserter commits.
    //
    // The real data-integrity invariant is the consumed_set check in
    // write_committed_change, which verifies at end-of-block that all
    // consumed entries were matched by inserts.
    live_entries = std::max(live_entries, int64_t{0});

    return cost_model->get_retrocausal_queue_cost(
        static_cast<uint64_t>(live_entries));
  };

  // If no delta has been applied to this lock, use the snapshot cost.
  const int128_t cost_before = (added_entries_.empty() && !consume_delta_)
                                   ? snapshot_cost
                                   : calculate_cost();

  auto const &delta = change.delta();

  // Apply inserts
  size_t elements_added = 0;
  if (!delta.insert.added_entries.empty()) {
    added_entries_.insert(added_entries_.end(),
                          delta.insert.added_entries.begin(),
                          delta.insert.added_entries.end());
    elements_added += delta.insert.added_entries.size();
  }

  // Apply consume
  if (delta.consume) {
    CHECK(!consume_delta_.has_value());
    consume_delta_ = *delta.consume;
  }

  int64_t element_delta =
      static_cast<int64_t>(elements_added) - change.consumed_past_entries_;
  if (delta.consume) {
    element_delta -= delta.consume->current_block_consumed.size();
  }
  committed_element_count_delta_ += element_delta;

  const int128_t cost_after = calculate_cost();

  return cost_after - cost_before;
}

HogDBStatus<void>
RetrocausalQueueLock::write_committed_change(LockContext const &ctx,
                                             MerkleTriePrefix &trie) const
{
  assert(ctx.dao);

  // Compute updated metadata
  RetrocausalQueueMetadata const &base_metadata = *get_meta(&ctx).value();
  RetrocausalQueueMetadata updated_metadata = base_metadata;

  if (consume_delta_) {
    auto const &updated_past_block_consume = consume_delta_->past_block_consume;

    // Prune fully consumed blocks.
    for (uint64_t b =
             base_metadata.consume_state.max_fully_consumed_block_number;
         b < updated_past_block_consume.max_fully_consumed_block_number; ++b) {
      const uint64_t block_num = b + 1;
      auto r = ctx.dao->remove_retrocausal_queue_block(ctx.key, block_num);
      if (!r.has_value()) {
        return r;
      }

      auto block_key = make_retrocausal_queue_block_key(ctx.key, block_num);
      std::span<const std::byte> merkle_key =
          MerkleTrieHelpers::add_type_to_merkle_key(
              std::as_bytes(std::span(block_key)).subspan(1), 0);
      CHECK(trie.remove(merkle_key));
    }
    updated_metadata.consume_state = updated_past_block_consume;
  }

  // Build the new block by filtering out consumed entries BY VALUE.
  // This is necessary because ItemView prioritizes Self entries over Concurrent
  // entries, so the consumed entries may not be at the front of added_entries_.
  x23::Vec<QueueEntry> remaining_entries;
  if (consume_delta_ && !consume_delta_->current_block_consumed.empty()) {
    // Create a multiset of consumed entries for efficient lookup
    std::multiset<QueueEntry> consumed_set(
        consume_delta_->current_block_consumed.begin(),
        consume_delta_->current_block_consumed.end());

    for (auto const &entry : added_entries_) {
      auto it = consumed_set.find(entry);
      if (it != consumed_set.end()) {
        // This entry was consumed; remove it from the set (to handle
        // duplicates)
        consumed_set.erase(it);
      }
      else {
        remaining_entries.push_back(entry);
      }
    }
    // All consumed entries must have been found among added entries.
    // In validation mode, a malicious proposer could fabricate observations
    // that don't match any inserted entries.  Return an error instead of
    // crashing so the block can be rejected gracefully.
    if (!consumed_set.empty()) {
      return HogDBError{HogDBCode::kInternal};
    }
  }
  else {
    // No current block consumption; all added entries remain
    remaining_entries =
        x23::Vec<QueueEntry>(added_entries_.begin(), added_entries_.end());
  }

  if (!remaining_entries.empty()) {
    RetrocausalQueueBlock new_block;
    new_block.entries = std::move(remaining_entries);
    std::ranges::sort(new_block.entries);
    new_block.block_number = base_metadata.max_live_block_num + 1;

    auto write_result =
        ctx.dao->write_retrocausal_queue_block(ctx.key, new_block);
    if (!write_result.has_value()) {
      return write_result.error();
    }

    auto block_key =
        make_retrocausal_queue_block_key(ctx.key, new_block.block_number);
    x23::Vec<uint8_t> block_serialized = x23::serialize(new_block);
    std::span<const std::byte> merkle_key =
        MerkleTrieHelpers::add_type_to_merkle_key(
            std::as_bytes(std::span(block_key)).subspan(1), 0);
    CHECK(trie.insert_or_update(merkle_key,
                                std::as_bytes(std::span(block_serialized))));

    updated_metadata.max_live_block_num = new_block.block_number;
  }

  updated_metadata.live_entries += committed_element_count_delta_;

  if (updated_metadata.live_entries == 0) {
    // Remove the item from the DAO and trie if it actually existed in the
    // snapshot. This includes two cases:
    // (1) base_metadata.live_entries > 0: an existing RCQ was fully consumed.
    // (2) base_was_different_type_: the key held a different type (e.g.
    //     bytestring) that was deleted and replaced with an RCQ in the same
    //     block. The old item must be removed even though no RCQ metadata
    //     existed.
    if (base_metadata.live_entries > 0) {
      // Clean up all past blocks before removing metadata.
      // Use updated_metadata to account for consumes in this transaction.
      //
      // Note: We don't use cleanup_on_deletion here because that function
      // reads metadata from the DAO (which still has the old consume state),
      // while we need to use updated_metadata which reflects this
      // transaction's consume operations
      uint64_t start_block =
          updated_metadata.consume_state.max_fully_consumed_block_number + 1;
      for (uint64_t b = start_block; b <= base_metadata.max_live_block_num;
           ++b) {
        auto block_remove = ctx.dao->remove_retrocausal_queue_block(ctx.key, b);
        if (!block_remove.has_value()) {
          return block_remove.error();
        }

        auto block_key = make_retrocausal_queue_block_key(ctx.key, b);
        std::span<const std::byte> merkle_key =
            MerkleTrieHelpers::add_type_to_merkle_key(
                std::as_bytes(std::span(block_key)).subspan(1), 0);
        CHECK(trie.remove(merkle_key));
      }

      auto meta_remove = ctx.dao->remove_retrocausal_queue_metadata(ctx.key);
      if (!meta_remove.has_value()) {
        return meta_remove.error();
      }

      CHECK(MerkleTrieHelpers::remove_kv_pair_from_trie(trie, ctx.key,
                                                        kRetrocausalQueue)
                .has_value());
    }
    else if (base_was_different_type_) {
      // The key held a different type (e.g. bytestring) that was deleted and
      // replaced with an RCQ in the same block. Look up the actual type to
      // remove the correct trie entry.
      auto type_ret = ctx.dao->get_item_type(ctx.key);
      if (!type_ret.has_value()) {
        return type_ret.error();
      }

      auto remove_ret = ctx.dao->remove_item(ctx.key);
      if (!remove_ret.has_value()) {
        return remove_ret.error();
      }

      CHECK(MerkleTrieHelpers::remove_kv_pair_from_trie(trie, ctx.key,
                                                        type_ret.value())
                .has_value());
    }
  }
  else {
    auto write_meta =
        ctx.dao->write_retrocausal_queue_metadata(ctx.key, updated_metadata);
    if (!write_meta.has_value()) {
      return write_meta.error();
    }

    x23::Vec<uint8_t> buf = x23::serialize(updated_metadata);
    CHECK(MerkleTrieHelpers::update_kv_pair_in_trie(
              trie, ctx.key, kRetrocausalQueue, std::as_bytes(std::span(buf)))
              .has_value());
  }

  return {};
}

std::optional<QueueEntry>
RetrocausalQueueLock::get_concurrent_entry(uint32_t index) const
{
  std::lock_guard lock(mtx_);

  if (index < added_entries_.size()) {
    return added_entries_[index];
  }
  return std::nullopt;
}

HogDBStatus<void>
RetrocausalQueueLock::cleanup_on_deletion(const LockContext &ctx,
                                          MerkleTriePrefix &trie)
{
  assert(ctx.dao);

  // Read the RCQ metadata to find all blocks that need cleanup
  auto meta_ret = ctx.dao->read_retrocausal_queue_metadata(ctx.key);
  if (!meta_ret.has_value()) {
    // If metadata doesn't exist, it's a no-op (deleting non-existent RCQ)
    if (meta_ret.error() == HogDBCode::kNotFound) {
      return {};
    }
    return meta_ret.error();
  }

  auto const &meta = meta_ret.value();

  // Remove all unconsumed blocks from both the DAO and the merkle trie
  uint64_t start_block = meta.consume_state.max_fully_consumed_block_number + 1;
  for (uint64_t b = start_block; b <= meta.max_live_block_num; ++b) {
    auto block_remove = ctx.dao->remove_retrocausal_queue_block(ctx.key, b);
    if (!block_remove.has_value()) {
      return block_remove.error();
    }

    auto block_key = make_retrocausal_queue_block_key(ctx.key, b);
    std::span<const std::byte> merkle_key =
        MerkleTrieHelpers::add_type_to_merkle_key(
            std::as_bytes(std::span(block_key)).subspan(1), 0);
    CHECK(trie.remove(merkle_key));
  }

  // Remove the metadata from the DAO and trie
  auto meta_remove = ctx.dao->remove_retrocausal_queue_metadata(ctx.key);
  if (!meta_remove.has_value()) {
    return meta_remove.error();
  }

  CHECK(MerkleTrieHelpers::remove_kv_pair_from_trie(trie, ctx.key,
                                                    kRetrocausalQueue)
            .has_value());

  return {};
}

} // namespace hogd
