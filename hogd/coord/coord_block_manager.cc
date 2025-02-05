#include "hogd/coord/coord_block_manager.h"

#include <cassert>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/coord/block_frame.h"
#include "hogd/coord/database_manager.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

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
  block_hash_to_id_.emplace(kGenesisBlockHash, last_committed_block_id_);
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
  throw std::runtime_error("invalid state");
}

CoordBlock::CoordBlock(BlockID block_id)
  : block_id_(block_id), current_frame_(std::in_place_type<BlockProposalFrame>),
    state_(CoordBlockState::kOpenForProposal)
{}

CoordBlock::CoordBlock(BlockID block_id, std::vector<ProposedTransaction> txs,
                       SHA256Hash block_hash)
  : block_id_(block_id),
    current_frame_(std::in_place_type<BlockValidationFrame>, std::move(txs),
                   block_hash),
    state_(CoordBlockState::kValidating)
{}

SHA256Hash
CoordBlock::canonical_block_hash() const
{
  if (std::holds_alternative<BlockProposalFrame>(current_frame_)) {
    throw std::runtime_error("unimplemented");
  }
  assert(std::holds_alternative<BlockValidationFrame>(current_frame_));
  return std::get<BlockValidationFrame>(current_frame_).canonical_block_hash();
}

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

  database_manager_.open_block(parent_id, block_id)();

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
    BlockID parent_id, std::vector<ProposedTransaction> txs,
    SHA256Hash const &block_hash)
{
  std::lock_guard lock(mtx_);
  auto it = open_block_guard(parent_id);
  if (it == blocks_.end()) {
    return std::nullopt;
  }

  auto &parent_entry = it->second;

  auto block_id = allocate_block_id();
  parent_entry.children_.push_back(block_id);

  database_manager_.open_block(parent_id, block_id)();

  blocks_.emplace(block_id, BlockEntry{
                                .block_ = std::make_shared<CoordBlock>(
                                    block_id, std::move(txs), block_hash),
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
  database_manager_.commit_block(block_id)();

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
CoordinatorBlockManager::prune_block(BlockID block_id)
{
  std::lock_guard lock(mtx_);

  auto it = blocks_.find(block_id);
  if (it == blocks_.end()) {
    return;
  }
  assert(!it->second.block_->committed());
  // hasn't been closed
  assert(block_hash_to_id_.find(it->second.block_->canonical_block_hash()) ==
         block_hash_to_id_.end());

  remove_children(it->second.children_);
  blocks_.erase(it);
}

std::optional<BlockID>
CoordinatorBlockManager::lookup_block_id(SHA256Hash hash) const
{
  auto it = block_hash_to_id_.find(hash);
  if (it == block_hash_to_id_.end()) {
    return std::nullopt;
  }
  return it->second;
}

x23::Async<std::optional<SHA256Hash>>
CoordinatorBlockManager::close_block(BlockID block_id)
{
  auto block_close_res = database_manager_.close_block(block_id);

  SHA256Hash hash;
  {
    std::lock_guard lock(mtx_);
    auto it = blocks_.find(block_id);
    assert(it != blocks_.end());
    auto &block = *it->second.block_;

    hash = block.canonical_block_hash();
    block_hash_to_id_[hash] = block_id;
  }

  if (!co_await block_close_res) {
    co_return std::nullopt;
  }
  co_return hash;
}

void
CoordinatorBlockManager::remove_children(std::vector<BlockID> children)
{
  while (!children.empty()) {
    auto child = children.back();
    children.pop_back();

    auto it = blocks_.find(child);
    if (it == blocks_.end()) {
      // This might happen if a block is pruned
      continue;
    }
    assert(!it->second.block_->committed());
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
