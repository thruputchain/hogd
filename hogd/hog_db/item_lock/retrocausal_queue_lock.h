#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/item_lock/pointer_lock.h"
#include "hogd/hog_db/status.h"
#include "retrocausal_queue.hh"
#include "x23/testlock.h"

namespace hogd {

// No new inserts are admitted once a retrocausal queue's committed
// (block-start) size has reached this bound; see
// RetrocausalQueueLock::try_atomic_change_lock.
inline constexpr uint64_t kMaxRetrocausalQueueLiveEntries =
    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) -
    kRetrocausalQueueBlockEntriesMaxLen;

// Within a single block, live_entries may overshoot the bound above by at
// most one block of inserts (per-block growth is capped at
// kRetrocausalQueueBlockEntriesMaxLen), giving this hard ceiling on any
// legitimately persisted value. HogDbDAO CHECKs it on every metadata load.
// live_entries is persisted as an unsigned XDR hyper, but storage-cost
// accounting (StorageCostModel::get_retrocausal_queue_cost and the int64_t
// arithmetic in RetrocausalQueueLock) is signed 64-bit; the static_assert
// keeps every admissible count inside the signed domain (issue #2917).
inline constexpr uint64_t kRetrocausalQueueLiveEntriesHardCap =
    kMaxRetrocausalQueueLiveEntries + kRetrocausalQueueBlockEntriesMaxLen;

static_assert(kRetrocausalQueueLiveEntriesHardCap <=
              static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));

// A single explicit deletion of a retrocausal queue expands at block close into
// one ChangeSet::remove per live block row plus the metadata row (see
// RetrocausalQueueLock::cleanup_on_deletion, driven from
// DeletionLock::stage_committed_changes). Every RCQ block row is an independent
// consensus state item (Prefix::kRetrocausalQueueBlock, folded into the
// StorageLongHash), so that per-row removal is irreducible: it must read RAMDB,
// update the hash delta, and stage a materialized remove for each row. A
// constant-size delete admitted with no range-dependent charge would otherwise
// force that fan-out over the queue's entire retained live-block history.
//
// Bound it: reject a deletion at admission when the queue still spans more than
// this many live block rows, so one transaction can never concentrate more than
// a fixed amount of cleanup work into block close. This is a consensus-uniform
// per-transaction resource limit in the same family as
// kMaxConcurrentRcqReadsPerTx and kMaxRetrocausalQueueLiveEntries (every node
// derives the same live-block count from committed metadata and agrees on the
// verdict). A contract holding a larger queue can consume it down below the
// bound before deleting. Changing this value is a coordinated protocol change.
inline constexpr uint64_t kMaxRetrocausalQueueBlocksPerDeletion = 16384;

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

  HogDbStatus<int128_t> try_atomic_change_lock(const change_type *before,
                                               const change_type *after,
                                               const LockContext &ctx);

  int128_t commit_change(bool did_finalize, int128_t snapshot_cost,
                         change_type &change, const LockContext &ctx);

  HogDbStatus<void> stage_committed_changes(const LockContext &ctx,
                                            ChangeSet &changes) const;

  std::optional<QueueEntry> get_concurrent_entry(uint32_t index) const;

  HogDbStatus<RetrocausalQueueMetadata *>
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
  static HogDbStatus<void> cleanup_on_deletion(const LockContext &ctx,
                                               ChangeSet &changes,
                                               bool keep_metadata = false);

  // Admission-time guard for an explicit deletion at ctx.key. Returns kTooBig
  // when the item is a retrocausal queue whose block-close cleanup
  // (cleanup_on_deletion) would remove more than
  // kMaxRetrocausalQueueBlocksPerDeletion live block rows; otherwise {}
  // (including for a non-RCQ key, a missing item, or a null DAO). Must be
  // called from every deletion admission path -- DeletionLock and the
  // ExclusiveLock deletion branch -- so an over-budget delete fails as a
  // transaction during execution rather than reaching the commit-time
  // cleanup, where the error would poison the whole block.
  static HogDbStatus<void> check_deletion_block_budget(const LockContext &ctx);

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
