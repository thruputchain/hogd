#include "hogd/hogdb/storage_item/retrocausal_queue.h"
#include "storage_item.hh"

#include <cassert>
#include <utility>

namespace hogd {

RetrocausalQueue::RetrocausalQueue(
    RetrocausalQueue const &parent,
    RetrocausalQueuePastBlockConsumeDelta const &consume_delta,
    RetrocausalQueueBlock current_round_block)
  : current_block_number_(current_round_block.block_number)
{
  for (auto const &[block_number, block] : parent.blocks_) {
    assert(block_number != current_round_block.block_number);
    blocks_.emplace(block_number, CoWBlockReference{.block_ = block.block_,
                                                    .is_modified_ = false});
  }
  apply_consume_delta(consume_delta);
  [[maybe_unused]] auto res = blocks_.emplace(
      current_round_block.block_number,
      CoWBlockReference{.block_ = std::make_shared<RetrocausalQueueBlock>(
                            std::move(current_round_block)),
                        .is_modified_ = true});
  assert(res.second);
  assert(current_block_number_ > parent.current_block_number_);
}

RetrocausalQueue::RetrocausalQueue(
    RetrocausalQueue const &parent,
    RetrocausalQueuePastBlockConsumeDelta const &consume_delta)
  : current_block_number_(parent.current_block_number_ + 1)
{
  for (auto const &[block_number, block] : parent.blocks_) {
    blocks_.emplace(block_number, CoWBlockReference{.block_ = block.block_,
                                                    .is_modified_ = false});
  }
  apply_consume_delta(consume_delta);
  // Not strictly necessary.  The current block number need only be
  // monotonically increasing when there's actually a block.  It should never be
  // exposed to the runtime anyways.
  assert(current_block_number_ > parent.current_block_number_);
}

RetrocausalQueue::RetrocausalQueue(uint64_t current_block_number)
  : current_block_number_(current_block_number)
{}

RetrocausalQueueBlock &
RetrocausalQueue::get_block(uint64_t block_number)
{
  auto it = blocks_.find(block_number);
  if (it != blocks_.end()) {
    // no-op if already refreshed
    it->second.refresh_for_cow();
    return *it->second.block_;
  }

  blocks_.emplace(
      block_number,
      CoWBlockReference{.block_ = std::make_shared<RetrocausalQueueBlock>(
                            x23::Vec<QueueEntry>{}, block_number),
                        .is_modified_ = true});
  return *blocks_.at(block_number).block_;
}

void
RetrocausalQueue::CoWBlockReference::refresh_for_cow()
{
  if (is_modified_) {
    return;
  }
  block_ = std::make_shared<RetrocausalQueueBlock>(*block_);
  is_modified_ = true;
}

const RetrocausalQueueBlock *
RetrocausalQueue::try_get_next_block(uint64_t search_start_number)
{
  auto it = blocks_.lower_bound(search_start_number);
  if (it == blocks_.end()) {
    return nullptr;
  }
  return it->second.block_.get();
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
    it = blocks_.erase(it);
  }

  if (delta.partial_consumed_number > 0) {
    assert(it != blocks_.end());
    it->second.refresh_for_cow();
    it->second.block_->entries = {it->second.block_->entries.begin() +
                                      delta.partial_consumed_number,
                                  it->second.block_->entries.end()};
  }
}

} // namespace hogd
