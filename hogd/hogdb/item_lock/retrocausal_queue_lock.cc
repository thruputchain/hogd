#include "hogd/hogdb/item_lock/retrocausal_queue_lock.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
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
    return ctx.cost_model->get_retrocausal_queue_cost(meta.live_entries,
                                                      live_block_count(meta));
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

  // Compute number of blocks added by this transaction. 0 if no inserts,
  // otherwise all new insertions go into the same block.
  // Consume may not free up any blocks, so conservatively we can just ignore
  // consumes for the initial storage cost adjustments. Later we could be more
  // generous. By looking at HogDB we could find the blocks that would be freed
  // by the consume and adjust the storage cost accordingly. Instead,
  // transactions that consume won't get access to that returned storage cost
  // until they commit. Hence: we don't need to account for consume in the
  // return cost bound.
  const uint64_t added_blocks = delta.insert.added_entries.empty() ? 0 : 1;

  const size_t new_live_entries =
      meta.live_entries + delta.insert.added_entries.size();
  const uint64_t new_live_blocks = live_block_count(meta) + added_blocks;

  return ctx.cost_model->get_retrocausal_queue_cost(new_live_entries,
                                                    new_live_blocks);
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
    uint64_t head_block =
        consume_delta_
            ? consume_delta_->past_block_consume.max_fully_consumed_block_number
            : meta.consume_state.max_fully_consumed_block_number;
    uint64_t blocks = (meta.max_live_block_num > head_block)
                          ? (meta.max_live_block_num - head_block)
                          : 0;

    // Only add a block if entries will remain after filtering consumed ones.
    // This matches the logic in write_committed_change which only writes a
    // new block if remaining_entries is non-empty.
    size_t consumed_from_current_block =
        consume_delta_ ? consume_delta_->current_block_consumed.size() : 0;
    if (added_entries_.size() > consumed_from_current_block)
      blocks += 1;

    int64_t live_entries = meta.live_entries + committed_element_count_delta_;
    assert(live_entries >= 0);

    return cost_model->get_retrocausal_queue_cost(static_cast<uint64_t>(
                                                      live_entries),
                                                  blocks);
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
    RetrocausalQueuePastBlockConsumeDelta const &total_past_block_consume =
        consume_delta_->past_block_consume;

    // Prune fully consumed blocks.
    for (uint64_t b =
             base_metadata.consume_state.max_fully_consumed_block_number;
         b < total_past_block_consume.max_fully_consumed_block_number; ++b) {
      CHECK(
          ctx.dao->remove_retrocausal_queue_block(ctx.key, b + 1).has_value());
    }
    updated_metadata.consume_state = total_past_block_consume;
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
    // Sanity check: all consumed entries should have been found
    CHECK(consumed_set.empty());
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
    CHECK(
        ctx.dao->write_retrocausal_queue_block(ctx.key, new_block).has_value());
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
      CHECK(ctx.dao->remove_retrocausal_queue_metadata(ctx.key).has_value());
      CHECK(MerkleTrieHelpers::remove_kv_pair_from_trie(trie, ctx.key,
                                                        kRetrocausalQueue)
                .has_value());
    }
    else if (base_was_different_type_) {
      // The key held a different type (e.g. bytestring) that was deleted and
      // replaced with an RCQ in the same block. Look up the actual type to
      // remove the correct trie entry.
      auto type_ret = ctx.dao->get_item_type(ctx.key);
      CHECK(type_ret.has_value());
      CHECK(ctx.dao->remove_item(ctx.key).has_value());
      CHECK(MerkleTrieHelpers::remove_kv_pair_from_trie(trie, ctx.key,
                                                        type_ret.value())
                .has_value());
    }
  }
  else {
    CHECK(ctx.dao->write_retrocausal_queue_metadata(ctx.key, updated_metadata)
              .has_value());
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

} // namespace hogd
