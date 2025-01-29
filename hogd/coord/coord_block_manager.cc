#include "hogd/coord/coord_block_manager.h"

#include <cassert>

#include "hogd/coord/block_frame.h"

namespace hogd {

CoordinatorBlockManager::CoordinatorBlockManager(
    DatabaseManager &database_manager)
  : database_manager_(database_manager)
{
  blocks_.emplace(last_committed_block_id_,
                  BlockEntry{
                      .block_ = nullptr,
                      .children_ = {},
                      .closed_to_new_children_ = false,
                  });
}

bool
CoordBlock::ready_for_commit() const
{
  if (std::holds_alternative<BlockProposalFrame>(current_frame_)) {
    switch (state_) {
    case CoordBlockState::kOpenForProposal:
      return false;
    case CoordBlockState::kProposalClosed:
      return true;
    default:
      assert(false);
    }
  }

  assert(std::holds_alternative<BlockValidationFrame>(current_frame_));
  switch (state_) {
  case CoordBlockState::kValidating:
    return false;
  case CoordBlockState::kValidateSuccess:
    return true;
  case CoordBlockState::kValidateFailure:
    return false;
  default:
    assert(false);
  }
}

CoordBlock::CoordBlock(BlockID block_id)
  : block_id_(block_id), current_frame_(std::in_place_type<BlockProposalFrame>),
    state_(CoordBlockState::kOpenForProposal)
{}

CoordBlock::CoordBlock(BlockID block_id, std::vector<ProposedTransaction> txs)
  : block_id_(block_id),
    current_frame_(std::in_place_type<BlockValidationFrame>, std::move(txs)),
    state_(CoordBlockState::kValidating)
{}

std::unordered_map<BlockID, CoordinatorBlockManager::BlockEntry>::iterator
CoordinatorBlockManager::open_block_guard(BlockID parent_id)
{
  auto it = blocks_.find(parent_id);
  if (it == blocks_.end()) {
    return blocks_.end();
  }
  if (it->second.closed_to_new_children_) {
    return blocks_.end();
  }
  return it;
}

std::optional<BlockID>
CoordinatorBlockManager::open_proposal(BlockID parent_id)
{
  std::lock_guard lock(mtx_);
  auto it = open_block_guard(parent_id);
  if (it == blocks_.end()) {
    return std::nullopt;
  }

  auto &parent_entry = it->second;
  auto block_id = allocate_block_id();
  parent_entry.children_.push_back(block_id);

  database_manager_.open_block(parent_id, block_id);

  blocks_.emplace(block_id,
                  BlockEntry{
                      .block_ = std::make_shared<CoordBlock>(block_id),
                      .children_ = {},
                      .closed_to_new_children_ = false,
                  });

  return block_id;
}

std::optional<BlockID>
CoordinatorBlockManager::add_block_to_validate(
    BlockID parent_id, std::vector<ProposedTransaction> txs)
{
  std::lock_guard lock(mtx_);
  auto it = open_block_guard(parent_id);
  if (it == blocks_.end()) {
    return std::nullopt;
  }

  auto &parent_entry = it->second;

  auto block_id = allocate_block_id();
  parent_entry.children_.push_back(block_id);

  database_manager_.open_block(parent_id, block_id);

  blocks_.emplace(block_id,
                  BlockEntry{
                      .block_ = std::make_shared<CoordBlock>(block_id,
                                                             std::move(txs)),
                      .children_ = {},
                      .closed_to_new_children_ = false,
                  });

  return block_id;
}

void
CoordinatorBlockManager::commit_block(BlockID block_id)
{
  std::lock_guard lock(mtx_);

  // commit block_id in the database
  auto it = blocks_.find(block_id);
  assert(it != blocks_.end());
  assert(it->second.block_);
  auto &block = *it->second.block_;
  assert(!block.committed());
  assert(block.ready_for_commit());
  block.mark_as_committed();
  database_manager_.commit_block(block_id);

  // cleanup sibling blocks
  it = blocks_.find(last_committed_block_id_);
  assert(it != blocks_.end());

  auto &last_committed_entry = it->second;
  assert(!last_committed_entry.closed_to_new_children_);
  last_committed_entry.closed_to_new_children_ = true;

  std::vector<BlockID> children_to_remove = last_committed_entry.children_;
  [[maybe_unused]] bool found_self_id = false;
  for (size_t i = 0; i < children_to_remove.size(); ++i) {
    if (children_to_remove[i] == block_id) {
      found_self_id = true;
      children_to_remove[i] = children_to_remove.back();
      children_to_remove.pop_back();
      break;
    }
  }
  last_committed_entry.children_ = {block_id};
  assert(found_self_id);
  remove_children(children_to_remove);

  // TODO(geoff): DO we want to remove the entry for the previous
  // last_committed_block_id_ here or later?  Should presumably make the same
  // choice here and in the db.

  last_committed_block_id_ = block_id;
}

void
CoordinatorBlockManager::remove_children(std::vector<BlockID> children)
{
  while (!children.empty()) {
    auto child = children.back();
    children.pop_back();

    auto it = blocks_.find(child);
    assert(it != blocks_.end());
    auto entry = it->second;
    children.insert(children.end(), entry.children_.begin(),
                    entry.children_.end());
    blocks_.erase(it);
  }
}

std::shared_ptr<CoordBlock>
CoordinatorBlockManager::get_block(BlockID block_id)
{
  std::lock_guard lock(mtx_);
  auto it = blocks_.find(block_id);
  if (it == blocks_.end()) {
    return nullptr;
  }
  return it->second.block_;
}

} // namespace hogd
