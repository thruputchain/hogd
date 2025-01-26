#pragma once

#include <map>
#include <memory>

#include "hogd/base/types.h"
#include "storage_item.hh"

namespace hogd {

// copy on write interface to a retrocausal queue
// None of the methods here are thread-safe.
//
// This implementation, for convenience, assumes that block number 0 is never
// instantiated.  Every consume_entry can therefore set 0 as the
// past_block_consume.max_fully_consumed_block_number, to be a no-op.
// This could be fixed by making max_fully_consumed_block_number an optional.
//
// Note: The notion of "block number" here does not have to correspond to the
// notion of block number on the main chain.  It has to monotonically increase
// from one block to a subsequent one _when modifications are made_, but if the
// object is not modified then it need not change.  This notion of block number
// is used for internal tracking of which entries are consumed, and should
// (probably) not be made visible to contracts.
class RetrocausalQueue {
public:
  RetrocausalQueue(RetrocausalQueue const &parent,
                   RetrocausalQueuePastBlockConsumeDelta const &consume_delta,
                   RetrocausalQueueBlock current_round_block);
  RetrocausalQueue(uint64_t current_block_number);

  // allow move and copy
  RetrocausalQueue(RetrocausalQueue &&) = default;
  RetrocausalQueue &operator=(RetrocausalQueue &&) noexcept = default;
  RetrocausalQueue(RetrocausalQueue const &) = default;
  RetrocausalQueue &operator=(RetrocausalQueue const &) noexcept = default;

  // Returns the next block in the queue, or nullptr if the queue is empty.
  // Does not return current block entries.
  // Starts searching from block number search_start_number.
  // Does not refresh the block for copy-on-write.
  const RetrocausalQueueBlock *try_get_next_block(uint64_t search_start_number);

  uint64_t get_current_block_number() const { return current_block_number_; }

private:
  struct CoWBlockReference {
    std::shared_ptr<RetrocausalQueueBlock> block_;
    bool is_modified_ = false;

    void refresh_for_cow();
  };

  // Applies a consume delta to the retrocausal queue.
  // The delta is applied atomically.
  // It does not make sense to apply two consume deltas in the same block,
  // given that these queues are single-consumer multi-producer.
  void apply_consume_delta(RetrocausalQueuePastBlockConsumeDelta const &delta);

  RetrocausalQueueBlock &get_block(uint64_t block_number);

  std::map<uint64_t, CoWBlockReference> blocks_;
  uint64_t current_block_number_;
  bool committed_consume_ = false;
};

} // namespace hogd
