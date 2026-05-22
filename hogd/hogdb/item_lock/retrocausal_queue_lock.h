#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
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

[[nodiscard]] inline size_t
hash_queue_entry(const QueueEntry &entry)
{
  return SipHash24::get_default()(
      std::as_bytes(std::span(entry.data(), entry.size())));
}

class QueueEntryKey {
public:
  explicit QueueEntryKey(const QueueEntry &entry)
    : borrowed_entry_(&entry), hash_(hash_queue_entry(entry))
  {}

  QueueEntryKey(const QueueEntryKey &other)
    : owned_entry_(other.entry()), hash_(other.hash_)
  {}

  QueueEntryKey(QueueEntryKey &&other)
    : hash_(other.hash_)
  {
    if (other.borrowed_entry_) {
      owned_entry_ = *other.borrowed_entry_;
    }
    else {
      owned_entry_ = std::move(other.owned_entry_);
    }
  }

  QueueEntryKey &operator=(const QueueEntryKey &other)
  {
    if (this == &other) {
      return *this;
    }
    owned_entry_ = other.entry();
    borrowed_entry_ = nullptr;
    hash_ = other.hash_;
    return *this;
  }

  QueueEntryKey &operator=(QueueEntryKey &&other)
  {
    if (this == &other) {
      return *this;
    }
    if (other.borrowed_entry_) {
      owned_entry_ = *other.borrowed_entry_;
    }
    else {
      owned_entry_ = std::move(other.owned_entry_);
    }
    borrowed_entry_ = nullptr;
    hash_ = other.hash_;
    return *this;
  }

  [[nodiscard]] const QueueEntry &entry() const noexcept
  {
    return borrowed_entry_ ? *borrowed_entry_ : owned_entry_;
  }
  [[nodiscard]] size_t hash() const noexcept { return hash_; }

  friend bool operator==(const QueueEntryKey &lhs, const QueueEntryKey &rhs)
  {
    return lhs.hash_ == rhs.hash_ && lhs.entry() == rhs.entry();
  }

private:
  QueueEntry owned_entry_;
  const QueueEntry *borrowed_entry_{nullptr};
  size_t hash_;
};

struct QueueEntryKeyHasher {
  size_t operator()(const QueueEntryKey &entry) const noexcept
  {
    return entry.hash();
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
                         const change_type &change, const LockContext &ctx);

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

  // Tracks the balance between inserted and consumed current-block entries
  // across all committed TXs. Incremented for each inserted entry, decremented
  // for each consumed entry in current_block_consumed. At block close, any
  // entry with a negative count was consumed but never inserted — indicating
  // a malicious proposer.
  std::unordered_map<QueueEntryKey, int64_t, QueueEntryKeyHasher>
      consumed_set_balance_;

  void apply_consumed_set_balance_delta(const QueueEntry &entry,
                                        int64_t delta);

  // Returns true if any consumed current-block entry was not matched by an
  // insert across all committed TXs. Called by stage_committed_changes()
  // at block close time to detect malicious proposer fabrication.
  bool has_unmatched_consumes() const;

  mutable std::mutex mtx_;
};

static_assert(item_lock<RetrocausalQueueLock>);

} // namespace hogd
