#include "hogd/hogexec/interface/executor_state_manager.h"

#include <cassert>
#include <map>

#include "rpc.hh"

namespace hogd {

ExecutorStateManager::ExecutorContext *
ExecutorStateManager::get_executor_context(BlockID block_id)
{
  // Returns nullptr if blockId has not been initialized
  // No requirement that coordinator notify every executor
  // of every block.
  auto map = active_blocks_.take();
  if (auto it = map->find(block_id); it != map->end()) {
    return it->second.get();
  }
  return nullptr;
}

void
ExecutorStateManager::await_finish(BlockID block_id)
{
  auto map = active_blocks_.take();
  if (auto it = map->find(block_id); it != map->end()) {
    it->second->state_.await_finish();
    map->erase(it);
  }
  // This executor might not have run transactions on this block,
  // and so might not have opened the block on this executor
  // and might not have an entry.
}

void
ExecutorStateManager::open_proposal(BlockID new_proposal_id)
{
  active_blocks_->emplace(new_proposal_id, std::make_unique<ExecutorContext>(
                                               ExecutorMode::kProposing,
                                               new_proposal_id, db_proxy_));
}
void
ExecutorStateManager::open_validation(BlockID new_validation_id)
{
  active_blocks_->emplace(new_validation_id, std::make_unique<ExecutorContext>(
                                                 ExecutorMode::kValidating,
                                                 new_validation_id, db_proxy_));
}

} // namespace hogd
