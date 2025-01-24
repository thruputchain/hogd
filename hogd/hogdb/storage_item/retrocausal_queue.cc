#include "hogd/hogdb/storage_item/retrocausal_queue.h"

namespace hogd {

RetrocausalQueue::RetrocausalQueue(RetrocausalQueue const &parent,
                                   uint64_t current_block_number)
  : current_block_number_(current_block_number)
{
  for (auto const &[block_number, block] : parent.blocks_) {
    blocks_.emplace(block_number, CoWBlockReference{.block_ = block.block_,
                                                    .is_modified_ = false});
  }
  assert(current_block_number_ > 0);
}

RetrocausalQueue::RetrocausalQueue(uint64_t current_block_number)
  : current_block_number_(current_block_number)
{
  assert(current_block_number_ > 0);
}

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
RetrocausalQueue::apply_consume_delta(RetrocausalQueueConsumeDelta const &delta)
{
  assert(!committed_consume_);
  committed_consume_ = true;
  auto past_clear = delta.past_block_consume;

  auto it = blocks_.begin();
  while (it != blocks_.end() &&
         it->first <= past_clear.max_fully_consumed_block_number) {
    it = blocks_.erase(it);
  }

  if (delta.past_block_consume.partial_consumed_number > 0) {
    assert(it != blocks_.end());
    it->second.refresh_for_cow();
    it->second.block_->entries = {it->second.block_->entries.begin() +
                                      past_clear.partial_consumed_number,
                                  it->second.block_->entries.end()};
  }

  current_block_consumed_entries_ = delta.current_block_consumed;
}

void
RetrocausalQueue::apply_insert_delta(RetrocausalQueueInsertDelta const &delta)
{
  current_block_entries_.insert(current_block_entries_.end(),
                                delta.added_entries.begin(),
                                delta.added_entries.end());
}

void
RetrocausalQueue::canonicalize_and_commit_current_block()
{
  std::set<QueueEntry> entries;
  entries.insert(current_block_entries_.begin(), current_block_entries_.end());
  for (auto const &entry : current_block_consumed_entries_) {
    [[maybe_unused]] auto n_erased = entries.erase(entry);
    assert(n_erased == 1);
  }
  auto &block = get_block(current_block_number_);
  assert(block.entries.empty());
  for (auto const &entry : entries) {
    block.entries.push_back(entry);
  }
  // block.entries is sorted bc entries is a set
}

} // namespace hogd
