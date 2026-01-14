#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/pointer_lock.h"
#include "hogd/hogdb/status.h"
#include "retrocausal_queue.hh"
#include "x23/testlock.h"

namespace hogd {

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

  HogDBStatus<void> write_committed_change(LockContext const &ctx,
                                           MerkleTriePrefix &trie) const;

  std::optional<QueueEntry> get_concurrent_entry(uint32_t index) const;

  HogDBStatus<RetrocausalQueueMetadata *>
  get_meta(const LockContext *ctx) const;

  static uint64_t live_block_count(const RetrocausalQueueMetadata &meta)
  {
    return meta.max_live_block_num -
           meta.consume_state.max_fully_consumed_block_number;
  }

private:
  // Base metadata loaded from DAO at prepare_lock_contents time.
  // Contains consume_state (head), max_live_block_num (tail), live_entries.
  mutable std::atomic<RetrocausalQueueMetadata *> meta_;
  mutable std::optional<RetrocausalQueueMetadata> meta_cell_;
  mutable std::once_flag meta_once_flag_;

  // Used in try_atomic_change_lock to ensure MPSC semantics;
  // ensures no two transactions both lock a consume in the same block.
  std::atomic<bool> consumed_{false};

  // Used in commit_change to calculate the cost of the change and
  // stage all changes for writing. Not accessed concurrently, since
  // access is limited to commit_change and write_committed_change.
  std::vector<QueueEntry> added_entries_;
  std::optional<RetrocausalQueueConsumeDelta> consume_delta_;
  // Records how many entries consume_delta_ consumed. This is needed because
  // try_atomic_change_lock must access the DAO to determine how many entries
  // were consumed by the consume_delta_. commit_write() has no DAO access.
  // TODO(stutsman): Should we change the hogexec interface to have it pass
  // this information to avoid these accesses?
  size_t consumed_past_entries_{0};
  std::atomic<ssize_t> total_element_count_delta_{0};

  mutable x23::TestLock meta_init_mtx_;
  mutable x23::TestLock mtx_;
};

static_assert(item_lock<RetrocausalQueueLock>);

} // namespace hogd
