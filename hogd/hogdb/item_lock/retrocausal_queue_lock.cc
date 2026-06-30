#include "hogd/hogdb/item_lock/retrocausal_queue_lock.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <expected>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/change_set.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "retrocausal_queue.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

// RAII rollback helper local to try_atomic_change_lock. Holds a list of undo
// closures that run on destruction (in reverse order of registration) unless
// disarm() has been called. Not copyable, not movable: instances are strictly
// function-local and should not escape.
class ScopedUndo {
public:
  ScopedUndo() = default;
  ~ScopedUndo() noexcept
  {
    if (!armed_) {
      return;
    }
    // Run in reverse, mirroring stack-unwind semantics. The registered
    // closures only touch atomic counters and do not throw; if one ever
    // did, terminating here is preferable to leaving mutations uncommitted.
    for (auto &action : std::ranges::reverse_view(actions_)) {
      try {
        action();
      } catch (...) {
        panic("ScopedUndo rollback action threw; cannot safely recover");
      }
    }
  }

  ScopedUndo(const ScopedUndo &) = delete;
  ScopedUndo &operator=(const ScopedUndo &) = delete;
  ScopedUndo(ScopedUndo &&) = delete;
  ScopedUndo &operator=(ScopedUndo &&) = delete;

  void push(std::function<void()> action)
  {
    actions_.push_back(std::move(action));
  }

  void disarm() { armed_ = false; }

private:
  std::vector<std::function<void()>> actions_;
  bool armed_ = true;
};

} // namespace

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

    const size_t entries_to_unreserve = before->max_insert_count();
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
  ScopedUndo undo;
  auto const &delta = after->delta();

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
      undo.push([this] { consumed_.store(false, std::memory_order_relaxed); });
    }
  }
  else if (this_tx_already_consumed) {
    // Update removed the consume.
    consumed_.store(false, std::memory_order_relaxed);
    // NOTE: this undo is defensive. Today this branch is only reachable via
    // pop_subtransactions, whose lock call has new_entries <=
    // previous_entries (shrink) and thus never hits the overflow check -
    // no failure path exists downstream. RetrocausalQueueIt::narrow() in
    // item_change.cc blocks forward drops. The undo preserves the
    // "ScopedUndo owns rollback" invariant so any future edit that adds a
    // fallible step after this point stays correct automatically.
    undo.push([this] { consumed_.store(true, std::memory_order_relaxed); });
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
  const size_t new_entries_count = after->max_insert_count();
  const size_t previous_entries_count =
      (before != nullptr) ? before->max_insert_count() : 0;

  if (new_entries_count > previous_entries_count) {
    const size_t additional_entries =
        new_entries_count - previous_entries_count;
    const size_t reserved_before =
        reserved_entry_count_.fetch_add(additional_entries,
                                        std::memory_order_relaxed);
    // Register the unreserve undo once, unconditionally. On the overflow
    // branch ScopedUndo rolls this back for us on return; on the
    // fall-through it rolls back only if a later step fails.
    undo.push([this, additional_entries] {
      reserved_entry_count_.fetch_sub(additional_entries,
                                      std::memory_order_relaxed);
    });

    if (reserved_before + additional_entries >
        kRetrocausalQueueBlockEntriesMaxLen) {
      return HogDBError{HogDBCode::kConflict};
    }
  }
  else if (new_entries_count < previous_entries_count) {
    // Rollback case: unreserve entries that are no longer needed.
    //
    // This shrink branch is only reachable via rollback_to_savepoint, which
    // restores a previously-validated change. The consume validation block
    // below re-reads block sizes from the live DAO, so it CAN still fail here:
    // a committed sibling block may have pruned those blocks out from under us.
    // But that same sibling commit invalidates this BlockTX first, so such a
    // failure only ever happens on a block that is already doomed and will be
    // discarded. When it does, ScopedUndo's fetch_add may transiently push
    // reserved_entry_count_ back above kRetrocausalQueueBlockEntriesMaxLen and
    // cause a spurious kConflict for a concurrent transaction sharing this lock
    // — but that sibling transaction is in the same invalidated block and is
    // itself about to observe kStaleBlock, so the inflation is harmless. The
    // failure is surfaced to the caller by BlockTX::modify_lock (translating it
    // to kStaleBlock) and handled in InvokeTX::rollback_to_savepoint.
    const size_t entries_to_unreserve =
        previous_entries_count - new_entries_count;
    reserved_entry_count_.fetch_sub(entries_to_unreserve,
                                    std::memory_order_relaxed);
    undo.push([this, entries_to_unreserve] {
      reserved_entry_count_.fetch_add(entries_to_unreserve,
                                      std::memory_order_relaxed);
    });
  }

  if (delta.consume) {
    // Now that we know this transaction will commit, we need to do one more
    // thing before we compute storage costs. commit_change() should not
    // consult the DAO, but the number of entries consumed by this delta
    // depends on the size of stored blocks. Compute the number of entries
    // consumed by this delta here and record it in case this transaction
    // commits.
    size_t consumed_entries = 0;

    auto const &snapshot_consume = meta.consume_state;
    auto const &new_consume = delta.consume->past_block_consume;

    // Reject consumes that reference blocks beyond the current max live block.
    // No valid hogexec could ever generate such a consume; attempting to
    // consume persistent blocks that have not yet come into existence is
    // impossible even in validation mode.
    if (new_consume.max_fully_consumed_block_number > meta.max_live_block_num) {
      return HogDBError{HogDBCode::kInvalidExecutorBehavior};
    }

    // Reject backward full-block consume. The snapshot's consume state is
    // the committed block-start state, identical in both proposal and
    // validation mode. hogexec can only consume forward from the snapshot.
    if (new_consume.max_fully_consumed_block_number <
        snapshot_consume.max_fully_consumed_block_number) {
      return HogDBError{HogDBCode::kInvalidExecutorBehavior};
    }

    // If all live blocks are fully consumed, partial_consumed must be 0
    // (there's no next block to partially consume from).
    if (new_consume.max_fully_consumed_block_number ==
            meta.max_live_block_num &&
        new_consume.partial_consumed_number > 0) {
      return HogDBError{HogDBCode::kInvalidExecutorBehavior};
    }

    if (new_consume.max_fully_consumed_block_number ==
        snapshot_consume.max_fully_consumed_block_number) {
      // Reject backward partial_consumed within the same block.
      if (new_consume.partial_consumed_number <
          snapshot_consume.partial_consumed_number) {
        return HogDBError{HogDBCode::kInvalidExecutorBehavior};
      }

      // Verify partial doesn't exceed actual block size.
      if (new_consume.partial_consumed_number >
          snapshot_consume.partial_consumed_number) {
        auto block_num = snapshot_consume.max_fully_consumed_block_number + 1;
        auto block = ctx.dao->read_retrocausal_queue_block(ctx.key, block_num);
        if (!block.has_value()) {
          return block.error();
        }
        if (new_consume.partial_consumed_number > block->entries.size()) {
          return HogDBError{HogDBCode::kInvalidExecutorBehavior};
        }
      }

      consumed_entries += (new_consume.partial_consumed_number -
                           snapshot_consume.partial_consumed_number);
    }
    else {
      // First block (the one we were partially in)
      auto first_block_num =
          snapshot_consume.max_fully_consumed_block_number + 1;
      auto first_block =
          ctx.dao->read_retrocausal_queue_block(ctx.key, first_block_num);
      if (!first_block.has_value()) {
        return first_block.error();
      }
      consumed_entries += (first_block->entries.size() -
                           snapshot_consume.partial_consumed_number);

      // Intermediate blocks
      for (uint64_t b = first_block_num + 1;
           b <= new_consume.max_fully_consumed_block_number; ++b) {
        auto block = ctx.dao->read_retrocausal_queue_block(ctx.key, b);
        if (!block.has_value()) {
          return block.error();
        }
        consumed_entries += block->entries.size();
      }

      // Last block (the one we end up partially in, which is now the new fully
      // consumed point)
      consumed_entries += new_consume.partial_consumed_number;
    }

    after->consumed_past_entries_ = consumed_entries;
  }

  // Conservative reservation: assume insertion into an empty RCQ.
  // We can't rely on the snapshot credit (from prior-block entries) because
  // a concurrent TX in this block may have type-changed or deleted the item,
  // invalidating the snapshot baseline.
  //
  // For deltas that both insert and consume: current-block consumes reduce
  // the net entry count (they consume entries this TX inserted). Past-block
  // consumes are ignored since an empty queue has nothing from past blocks.
  //
  // We add item_lock_snapshot_cost so that snapshot_cost_transform (which
  // subtracts snapshot_cost_) yields exactly the standalone RCQ cost.
  // commit_change returns the actual marginal cost (which may be much less),
  // and the excess is refunded via released_cost in InvokeTX::commit.
  size_t net_entries = delta.insert.added_entries.size();
  if (delta.consume) {
    size_t current_consumed = delta.consume->current_block_consumed.size();
    net_entries =
        (net_entries > current_consumed) ? net_entries - current_consumed : 0;
  }
  int128_t standalone_cost =
      ctx.cost_model->get_retrocausal_queue_cost(net_entries);
  undo.disarm();
  return standalone_cost + ctx.item_lock_snapshot_cost;
}

int128_t
RetrocausalQueueLock::commit_change(bool /*did_finalize*/,
                                    int128_t snapshot_cost, change_type &change,
                                    const LockContext &ctx)
{
  // Per-TX demand: observations plus current-block consumes.  A destructive
  // consume uses up one inserted entry, so the same insert cannot also cover
  // an observation within the same TX.  Adding both into one map and taking
  // the max across TXs lets has_unmatched_entries() reject doctored proposals
  // where a single insert is reused for both a consume and a later peek.
  // TODO(geoff): reduce copies by using non-owning span keys.
  std::map<QueueEntry, uint32_t> per_tx_demand;
  for (auto const &entry : change.delta_.observed_concurrent_entries) {
    ++per_tx_demand[entry];
  }
  if (change.delta_.consume) {
    for (auto const &entry : change.delta_.consume->current_block_consumed) {
      ++per_tx_demand[entry];
    }
  }

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
    // The real data-integrity invariant is the entry_tracking_ check in
    // stage_committed_changes, which verifies at end-of-block that all
    // consumed and observed entries were matched by inserts.
    live_entries = std::max(live_entries, int64_t{0});

    return ctx.cost_model->get_retrocausal_queue_cost(
        static_cast<uint64_t>(live_entries));
  };

  // If no delta has been applied to this lock, use the snapshot cost.
  const int128_t cost_before = (added_entries_.empty() && !consume_delta_)
                                   ? snapshot_cost
                                   : calculate_cost();

  auto &delta = change.delta_;

  const size_t elements_added = delta.insert.added_entries.size();
  const bool has_consume = delta.consume != nullptr;
  const size_t current_consumed_count =
      has_consume ? delta.consume->current_block_consumed.size() : 0;

  // Release excess reservation from self-consume high-water mark.
  size_t excess_reserved = change.max_insert_count() - elements_added;
  if (excess_reserved > 0) {
    reserved_entry_count_.fetch_sub(excess_reserved, std::memory_order_relaxed);
  }

  // Track per-entry accounting while entries are still readable.
  for (auto const &entry : delta.insert.added_entries) {
    ++entry_tracking_[entry].insert_count;
  }
  if (has_consume) {
    for (auto const &entry : delta.consume->current_block_consumed) {
      ++entry_tracking_[entry].consume_count;
    }
  }
  for (auto const &[entry, count] : per_tx_demand) {
    auto &t = entry_tracking_[entry];
    t.observe_count = std::max(t.observe_count, count);
  }

  // Move inserts into lock-owned state.
  if (!delta.insert.added_entries.empty()) {
    added_entries_.insert(
        added_entries_.end(),
        std::make_move_iterator(delta.insert.added_entries.begin()),
        std::make_move_iterator(delta.insert.added_entries.end()));
  }

  // Move consume delta into lock-owned state.
  if (has_consume) {
    CHECK(!consume_delta_.has_value());
    consume_delta_.emplace(std::move(*delta.consume));
  }

  int64_t element_delta =
      static_cast<int64_t>(elements_added) - change.consumed_past_entries_;
  if (has_consume) {
    element_delta -= current_consumed_count;
  }
  committed_element_count_delta_ += element_delta;

  const int128_t cost_after = calculate_cost();

  return cost_after - cost_before;
}

bool
RetrocausalQueueLock::has_unmatched_entries() const
{
  return std::ranges::any_of(entry_tracking_, [](auto const &kv) {
    auto const &t = kv.second;
    return t.consume_count > t.insert_count || t.observe_count > t.insert_count;
  });
}

HogDBStatus<void>
RetrocausalQueueLock::stage_committed_changes(const LockContext &ctx,
                                              ChangeSet &changes) const
{
  CHECK(ctx.dao != nullptr);

  // The entry_tracking_ was maintained incrementally in commit_change().
  // Every consumed entry must have a matching insert, and every observed
  // entry must have been inserted at least once. A violation indicates a
  // malicious proposer. Return an error before any DAO mutation so the block
  // can be safely rejected.
  if (has_unmatched_entries()) {
    return HogDBError{HogDBCode::kInvalidExecutorBehavior};
  }

  RetrocausalQueueMetadata const &base_metadata = *get_meta(&ctx).value();
  RetrocausalQueueMetadata updated_metadata = base_metadata;

  if (consume_delta_) {
    auto const &updated_past_block_consume = consume_delta_->past_block_consume;

    // Prune fully consumed blocks.
    for (uint64_t b =
             base_metadata.consume_state.max_fully_consumed_block_number;
         b < updated_past_block_consume.max_fully_consumed_block_number; ++b) {
      const uint64_t block_num = b + 1;
      auto block_key = make_retrocausal_queue_block_key(ctx.key, block_num);
      changes.remove(block_key);
    }
    updated_metadata.consume_state = updated_past_block_consume;
  }

  // Build the new block by filtering out consumed entries BY VALUE.
  // This is necessary because ItemView prioritizes Self entries over Concurrent
  // entries, so the consumed entries may not be at the front of added_entries_.
  x23::Vec<QueueEntry> remaining_entries;
  if (consume_delta_ && !consume_delta_->current_block_consumed.empty()) {
    std::multiset<QueueEntry> consumed_set(
        consume_delta_->current_block_consumed.begin(),
        consume_delta_->current_block_consumed.end());

    for (auto const &entry : added_entries_) {
      auto it = consumed_set.find(entry);
      if (it != consumed_set.end()) {
        consumed_set.erase(it);
      }
      else {
        remaining_entries.push_back(entry);
      }
    }
    // consumed_set was already validated by has_unmatched_entries() in
    // stage_committed_changes(). This CHECK guards against logic errors
    // in the filtering — it should never fire.
    CHECK(consumed_set.empty());
  }
  else {
    remaining_entries =
        x23::Vec<QueueEntry>(added_entries_.begin(), added_entries_.end());
  }

  if (!remaining_entries.empty()) {
    RetrocausalQueueBlock new_block;
    new_block.entries = std::move(remaining_entries);
    std::ranges::sort(new_block.entries);
    new_block.block_number = base_metadata.max_live_block_num + 1;

    auto block_key =
        make_retrocausal_queue_block_key(ctx.key, new_block.block_number);
    auto block_buf = x23::serialize(new_block);
    changes.put(block_key, 0, block_buf);

    updated_metadata.max_live_block_num = new_block.block_number;
  }

  int64_t new_live = static_cast<int64_t>(updated_metadata.live_entries) +
                     committed_element_count_delta_;
  if (new_live < 0) {
    return HogDBError{HogDBCode::kInvalidExecutorBehavior};
  }
  updated_metadata.live_entries = static_cast<uint64_t>(new_live);

  auto meta_key = make_contract_data_key(ctx.key);

  if (updated_metadata.live_entries == 0) {
    // Remove the item from the DAO if it actually existed in the snapshot.
    // This includes two cases:
    // (1) base_metadata.live_entries > 0: an existing RCQ was fully consumed.
    // (2) base_was_different_type_: the key held a different type (e.g.
    //     bytestring) that was deleted and replaced with an RCQ in the same
    //     block. The old item must be removed even though no RCQ metadata
    //     existed.
    if (base_metadata.live_entries > 0) {
      // Clean up remaining blocks. Use updated_metadata to account for
      // consumes already pruned above.
      uint64_t start_block =
          updated_metadata.consume_state.max_fully_consumed_block_number + 1;
      for (uint64_t b = start_block; b <= base_metadata.max_live_block_num;
           ++b) {
        auto block_key = make_retrocausal_queue_block_key(ctx.key, b);
        changes.remove(block_key);
      }
      changes.remove(meta_key);
    }
    else if (base_was_different_type_) {
      // The key held a different type (e.g. bytestring) that was deleted and
      // replaced with an RCQ in the same block. Remove the old item.
      changes.remove(meta_key);
    }
  }
  else {
    auto meta_buf = x23::serialize(updated_metadata);
    changes.put(meta_key, kRetrocausalQueue, meta_buf);
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
                                          ChangeSet &changes,
                                          bool keep_metadata)
{
  CHECK(ctx.dao != nullptr);

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

  // Remove all unconsumed blocks.
  uint64_t start_block = meta.consume_state.max_fully_consumed_block_number + 1;
  for (uint64_t b = start_block; b <= meta.max_live_block_num; ++b) {
    auto block_key = make_retrocausal_queue_block_key(ctx.key, b);
    changes.remove(block_key);
  }

  // Remove the metadata unless the caller will overwrite it (type change).
  if (!keep_metadata) {
    auto meta_key = make_contract_data_key(ctx.key);
    changes.remove(meta_key);
  }

  return {};
}

} // namespace hogd
