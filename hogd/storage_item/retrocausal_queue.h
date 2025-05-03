#pragma once

#include <map>
#include <memory>

#include "hogd/base/types.h"
#include "retrocausal_queue.hh"

namespace hogd {

// Stats about the retrocausal queue
// used when computing storage costs
struct RetrocausalQueueStats {
  uint64_t active_block_count;
  uint64_t active_size;

  bool operator==(const RetrocausalQueueStats &) const = default;
};

// copy on write interface to a retrocausal queue
// None of the methods here are thread-safe.
//
// The RCQ divides itself into "blocks" -- each of the entries created during
// one block are stored in a single block.  The block number is monotonically
// increasing, although the block number stored here does not need to match
// the block number on the main chain.
//
// The design assumption is that each of these blocks will be stored as
// individual entries within the database (so they can be looked up by queue id
// + block number). This interface provides a logical view of the entire queue,
// as a single object. This interface is copy-on-write (the write in a block is
// either "insert new block" or "remove a bunch of old blocks, possibly
// partially removing one of them", which are well-structured so the underlying
// db writes should be minimal), so it should be fairly cheap to copy (when e.g.
// working with multiple blocks that might be inflight concurrently).
//
// This implementation assumes that block number 0 is never instantiated.
// Every consume_entry can therefore set 0 as the
// past_block_consume.max_fully_consumed_block_number, to be a no-op.
// This could be changed by making max_fully_consumed_block_number an optional.
//
// Note: The notion of "block number" here does not have to correspond to the
// notion of block number on the main chain.  It has to monotonically increase
// from one block to a subsequent one _when modifications are made_, but if the
// object is not modified then it need not change.  This notion of block number
// is used for internal tracking of which entries are consumed, and should
// (probably) not be made visible to contracts.
class RetrocausalQueue {
public:
  // Ctor if current block is not empty
  RetrocausalQueue(RetrocausalQueue const &parent,
                   RetrocausalQueuePastBlockConsumeDelta const &consume_delta,
                   RetrocausalQueueBlock current_round_block);

  // Ctor if current block is empty
  RetrocausalQueue(RetrocausalQueue const &parent,
                   RetrocausalQueuePastBlockConsumeDelta const &consume_delta);

  RetrocausalQueue(uint64_t current_block_number);

  // allow move and copy
  RetrocausalQueue(RetrocausalQueue &&) = default;
  RetrocausalQueue &operator=(RetrocausalQueue &&) noexcept = default;
  RetrocausalQueue(RetrocausalQueue const &) = default;
  RetrocausalQueue &operator=(RetrocausalQueue const &) noexcept = default;

  // Returns the next block in the queue, or nullptr if the queue is empty.
  // Does not return current block entries.
  // Starts searching from block number search_start_number.
  const RetrocausalQueueBlock *try_get_next_block(uint64_t search_start_number);

  uint64_t get_current_block_number() const { return current_block_number_; }

  // stats of the queue in its current state
  RetrocausalQueueStats get_active_stats() const;
  // stats of the queue in a hypothetical state _after_ delta is
  // applied. Does not account for any entries that might additionally be
  // created/consumed in a new block.
  RetrocausalQueueStats get_stats_after_consume(
      RetrocausalQueuePastBlockConsumeDelta const &delta) const;

private:
  struct BlockState {
    RetrocausalQueueBlock block_;
    uint64_t cumulative_prior_size_;
  };

  // Applies a consume delta to the retrocausal queue.
  // The delta is applied atomically.
  // It does not make sense to apply two consume deltas in the same block,
  // given that these queues are single-consumer multi-producer.
  void apply_consume_delta(RetrocausalQueuePastBlockConsumeDelta const &delta);

  std::map<uint64_t, std::shared_ptr<const BlockState>> blocks_;
  uint64_t current_block_number_;

  // Total number of entries that have ever been inserted into the queue.
  uint64_t total_size_;
  // Total number of entries that have been consumed from the queue.
  uint64_t consumed_size_;

  bool committed_consume_ = false;
};

} // namespace hogd
