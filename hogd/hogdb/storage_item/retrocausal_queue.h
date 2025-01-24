#pragma once

#include <map>
#include <memory>
#include <set>

#include "hogd/base/types.h"
#include "storage_item.hh"

namespace hogd {

// copy on write interface to a retrocausal queue
// None of the methods here are thread-safe.
class RetrocausalQueue {
public:
  RetrocausalQueue(RetrocausalQueue const &parent,
                   uint64_t current_block_number);

  RetrocausalQueue(uint64_t current_block_number);

  RetrocausalQueue(RetrocausalQueue &&) = delete;
  RetrocausalQueue &operator=(RetrocausalQueue &&) = delete;
  RetrocausalQueue &operator=(RetrocausalQueue const &) = delete;

  // Returns the next block in the queue, or nullptr if the queue is empty.
  // Does not return current block entries.
  // Starts searching from block number search_start_number.
  // Does not refresh the block for copy-on-write.
  const RetrocausalQueueBlock *try_get_next_block(uint64_t search_start_number);

  // Applies a consume delta to the retrocausal queue.
  // The delta is applied atomically.
  // It does not make sense to apply two consume deltas in the same block,
  // given that these queues are single-consumer multi-producer.
  void apply_consume_delta(RetrocausalQueueConsumeDelta const &delta);

  // Applies an insert delta to the retrocausal queue.
  // Not required to be unique in a block.
  void apply_insert_delta(RetrocausalQueueInsertDelta const &delta);

  // The order of elements here is NONDETERMINISTIC.
  std::vector<QueueEntry> const &get_current_block_entries() const
  {
    return current_block_entries_;
  }

  // sorts the entries in the current block.
  // This is not responsible for maintaining a per-block size limit.
  void canonicalize_and_commit_current_block();

private:
  struct CoWBlockReference {
    std::shared_ptr<RetrocausalQueueBlock> block_;
    bool is_modified_ = false;

    void refresh_for_cow();
  };

  RetrocausalQueueBlock &get_block(uint64_t block_number);

  std::map<uint64_t, CoWBlockReference> blocks_;
  const uint64_t current_block_number_;
  std::vector<QueueEntry> current_block_entries_;
  x23::Vec<QueueEntry> current_block_consumed_entries_;
  bool committed_consume_ = false;
};

} // namespace hogd
