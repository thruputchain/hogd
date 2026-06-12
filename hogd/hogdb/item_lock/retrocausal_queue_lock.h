#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/status.h"
#include "retrocausal_queue.hh"
#include "x23/testlock.h"

namespace hogd {

// Uses the process-wide random-keyed SipHash24 to defend against adversarial
// hash collision attacks on entry_tracking_.
struct QueueEntryHasher {
  size_t operator()(const QueueEntry &entry) const
  {
    return SipHash24::get_default()(
        std::as_bytes(std::span(entry.data(), entry.size())));
  }
};

class RetrocausalQueueLock {
public:
  using change_type = RetrocausalQueueIt;

  RetrocausalQueueLock() = default;

  HogDBStatus<int128_t> try_atomic_change_lock(const change_type *before,
                                               const change_type *after,
                                               const LockContext &ctx);

  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         change_type &change, const LockContext &ctx);

  HogDBStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

  std::optional<QueueEntry> get_concurrent_entry(uint32_t index) const;

  HogDBStatus<RetrocausalQueueMetadata *>
  get_meta(const LockContext *ctx) const;

  static uint64_t live_block_count(const RetrocausalQueueMetadata &meta)
  {
    return meta.max_live_block_num -
           meta.consume_state.max_fully_consumed_block_number;
  }

  // Cleanup all RCQ-specific storage (blocks and metadata) when an RCQ is
  // deleted. Stages removals into the given ChangeSet. When
  // keep_metadata is true, only block entries are removed (used during
  // type changes where the caller overwrites the metadata key).
  static HogDBStatus<void> cleanup_on_deletion(const LockContext &ctx,
                                               ChangeSet &changes,
                                               bool keep_metadata = false);

private:
  // Base metadata loaded from DAO at prepare_lock_contents time.
  // Contains consume_state (head), max_live_block_num (tail), live_entries.
  mutable std::atomic<RetrocausalQueueMetadata *> meta_;
  mutable std::optional<RetrocausalQueueMetadata> meta_cell_;
  mutable std::mutex meta_init_mtx_;

  // Set to true when get_meta() encounters a kTypeMismatch error, indicating
  // the key exists in the DAO as a different type (e.g. bytestring). Used by
  // stage_committed_changes to know it must remove the old item even when
  // no RCQ metadata existed in the snapshot.
  mutable bool base_was_different_type_{false};

  // Used in try_atomic_change_lock to ensure MPSC semantics;
  // ensures no two transactions both lock a consume in the same block.
  std::atomic<bool> consumed_{false};

  // Tracks the number of entries reserved by transactions in the current
  // block. This allows try_atomic_change_lock to account for concurrent
  // insertions when checking the block size limit.
  std::atomic<size_t> reserved_entry_count_{0};

  // Used in commit_change to calculate the cost of the change and
  // stage all changes for writing. Not accessed concurrently, since
  // access is limited to commit_change and stage_committed_changes.
  std::vector<QueueEntry> added_entries_;
  std::optional<RetrocausalQueueConsumeDelta> consume_delta_;
  // Change in entry count in the whole queue over the set of committed deltas.
  // This is used when calculating marginal storage costs for a committing delta
  // in commit_change.
  ssize_t committed_element_count_delta_{0};

  struct EntryTracking {
    uint32_t insert_count = 0;
    uint32_t consume_count = 0;
    uint32_t observe_count = 0;
  };

  // Tracks per-entry insert, consume, and observe counts across all committed
  // TXs in the current block. insert_count and consume_count are summed across
  // TXs; observe_count is the max per-TX demand (consumes + observations),
  // since a consumed entry cannot also cover an observation within the same TX.
  // At block close, every entry must have insert_count >= consume_count and
  // insert_count >= observe_count.
  std::unordered_map<QueueEntry, EntryTracking, QueueEntryHasher>
      entry_tracking_;

  // Returns true if any consumed current-block entry was not matched by an
  // insert, or any observed entry was never inserted. Called by
  // stage_committed_changes() at block close time.
  bool has_unmatched_entries() const;

  mutable std::mutex mtx_;
};

static_assert(item_lock<RetrocausalQueueLock>);

} // namespace hogd
