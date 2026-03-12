#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/status.h"
#include "retrocausal_queue.hh"
#include "x23/testlock.h"

namespace hogd {

struct QueueEntryHasher {
  size_t operator()(const QueueEntry &entry) const
  {
    return std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char *>(entry.data()),
                         entry.size()));
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
                         const change_type &change,
                         const StorageCostModel *cost_model);

  HogDBStatus<void> prepare_committed_changes(const LockContext &ctx,
                                              Prepared &prepared) const;

  std::optional<QueueEntry> get_concurrent_entry(uint32_t index) const;

  HogDBStatus<RetrocausalQueueMetadata *>
  get_meta(const LockContext *ctx) const;

  // Returns true if any consumed current-block entry was not matched by an
  // insert across all committed TXs. Must be called after all commit_change()
  // calls for the block have completed (i.e., at block close time).
  bool has_unmatched_consumes() const;

  static uint64_t live_block_count(const RetrocausalQueueMetadata &meta)
  {
    return meta.max_live_block_num -
           meta.consume_state.max_fully_consumed_block_number;
  }

  // Cleanup all RCQ-specific storage (blocks and metadata) when an RCQ is
  // deleted. This mutates DAO state only; db hash updates are handled in
  // apply_db_hash_delta paths.
  static HogDBStatus<void> cleanup_on_deletion(const LockContext &ctx);

private:
  // Base metadata loaded from DAO at prepare_lock_contents time.
  // Contains consume_state (head), max_live_block_num (tail), live_entries.
  mutable std::atomic<RetrocausalQueueMetadata *> meta_;
  mutable std::optional<RetrocausalQueueMetadata> meta_cell_;
  mutable std::mutex meta_init_mtx_;

  // Set to true when get_meta() encounters a kTypeMismatch error, indicating
  // the key exists in the DAO as a different type (e.g. bytestring). Used by
  // write_committed_change to know it must remove the old item even when no
  // RCQ metadata existed in the snapshot.
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
  // access is limited to commit_change and write_committed_change.
  std::vector<QueueEntry> added_entries_;
  std::optional<RetrocausalQueueConsumeDelta> consume_delta_;
  // Change in entry count in the whole queue over the set of committed deltas.
  // This is used when calculating marginal storage costs for a committing delta
  // in commit_change.
  ssize_t committed_element_count_delta_{0};

  // Tracks the balance between inserted and consumed current-block entries
  // across all committed TXs. Incremented for each inserted entry, decremented
  // for each consumed entry in current_block_consumed. At block close, any
  // entry with a negative count was consumed but never inserted — indicating
  // a malicious proposer.
  std::unordered_map<QueueEntry, int64_t, QueueEntryHasher>
      consumed_set_balance_;

  mutable std::mutex mtx_;
};

static_assert(item_lock<RetrocausalQueueLock>);

} // namespace hogd
