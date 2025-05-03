#include "hogd/storage_item/retrocausal_queue.h"

#include <cassert>
#include <memory>
#include <utility>

#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "retrocausal_queue.hh"
#include "x23/vec.h"

namespace hogd {

RetrocausalQueue::RetrocausalQueue(
    RetrocausalQueue const &parent,
    RetrocausalQueuePastBlockConsumeDelta const &consume_delta,
    RetrocausalQueueBlock current_round_block)
  : blocks_(parent.blocks_),
    current_block_number_(current_round_block.block_number),
    total_size_(parent.total_size_ + current_round_block.entries.size()),
    consumed_size_(parent.consumed_size_)
{
  if constexpr (is_debug_build()) {
    for (auto const &[block_number, _] : parent.blocks_) {
      assert(block_number != current_round_block.block_number);
    }
  }

  assert(consume_delta.max_fully_consumed_block_number !=
         current_round_block.block_number);
  apply_consume_delta(consume_delta);

  [[maybe_unused]] auto res =
      blocks_.emplace(current_block_number_,
                      std::make_shared<BlockState>(
                          BlockState{.block_ = std::move(current_round_block),
                                     .cumulative_prior_size_ =
                                         parent.total_size_}));
  assert(res.second);
  assert(current_block_number_ > parent.current_block_number_);
}

RetrocausalQueue::RetrocausalQueue(
    RetrocausalQueue const &parent,
    RetrocausalQueuePastBlockConsumeDelta const &consume_delta)
  : current_block_number_(parent.current_block_number_ + 1),
    total_size_(parent.total_size_), consumed_size_(parent.consumed_size_)
{
  for (auto const &[block_number, block] : parent.blocks_) {
    blocks_.emplace(block_number, block);
  }
  apply_consume_delta(consume_delta);
  // Not strictly necessary.  The current block number need only be
  // monotonically increasing when there's actually a block.  It should never be
  // exposed to the runtime anyways.
  assert(current_block_number_ > parent.current_block_number_);
}

RetrocausalQueue::RetrocausalQueue(uint64_t current_block_number)
  : current_block_number_(current_block_number), total_size_(0),
    consumed_size_(0)
{}

const RetrocausalQueueBlock *
RetrocausalQueue::try_get_next_block(uint64_t search_start_number)
{
  auto it = blocks_.lower_bound(search_start_number);
  if (it == blocks_.end()) {
    return nullptr;
  }
  return &it->second->block_;
}

void
RetrocausalQueue::apply_consume_delta(
    RetrocausalQueuePastBlockConsumeDelta const &delta)
{
  assert(!committed_consume_);
  committed_consume_ = true;

  auto it = blocks_.begin();
  while (it != blocks_.end() &&
         it->first <= delta.max_fully_consumed_block_number) {
    consumed_size_ += it->second->block_.entries.size();
    it = blocks_.erase(it);
  }

  if (delta.partial_consumed_number > 0) {
    assert(it != blocks_.end());
    auto new_entries = x23::Vec<QueueEntry>{};
    new_entries.reserve(it->second->block_.entries.size() -
                        delta.partial_consumed_number);
    for (auto i = delta.partial_consumed_number;
         i < it->second->block_.entries.size(); ++i) {
      new_entries.push_back(it->second->block_.entries[i]);
    }

    consumed_size_ += delta.partial_consumed_number;

    it->second = std::make_shared<BlockState>(
        BlockState{.block_ = {.entries = std::move(new_entries),
                              .block_number = it->second->block_.block_number},
                   .cumulative_prior_size_ =
                       it->second->cumulative_prior_size_ +
                       delta.partial_consumed_number});
    assert(it->second->cumulative_prior_size_ == consumed_size_);
  }
}

RetrocausalQueueStats
RetrocausalQueue::get_active_stats() const
{
  return RetrocausalQueueStats{.active_block_count = blocks_.size(),
                               .active_size = total_size_ - consumed_size_};
}

RetrocausalQueueStats
RetrocausalQueue::get_stats_after_consume(
    RetrocausalQueuePastBlockConsumeDelta const &delta) const
{
  auto it = blocks_.upper_bound(delta.max_fully_consumed_block_number);

  if (it == blocks_.end()) {
    assert(delta.partial_consumed_number == 0);

    return RetrocausalQueueStats{.active_block_count = 0, .active_size = 0};
  }
  assert(it->first > delta.max_fully_consumed_block_number);

  return RetrocausalQueueStats{
      .active_block_count = current_block_number_ - it->first + 1,
      .active_size = total_size_ - it->second->cumulative_prior_size_ -
                     delta.partial_consumed_number};
}

} // namespace hogd
