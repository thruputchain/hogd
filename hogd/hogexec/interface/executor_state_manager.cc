#include "hogd/hogexec/interface/executor_state_manager.h"

#include <cassert>
#include <map>

#include "rpc.hh"

namespace hogd {

ExecutorState *
ExecutorStateManager::get_executor_state(BlockID block_id)
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
    it->second->await_finish();
    map->erase(it);
  }
  // This executor might not have run transactions on this block,
  // so it might not have an entry.
}

void
ExecutorStateManager::open_proposal(BlockID new_proposal_id)
{
  active_blocks_->emplace(new_proposal_id, std::make_unique<ExecutorState>(
                                               ExecutorMode::kProposing));
}
void
ExecutorStateManager::open_validation(BlockID new_validation_id)
{
  active_blocks_->emplace(new_validation_id, std::make_unique<ExecutorState>(
                                                 ExecutorMode::kValidating));
}

} // namespace hogd
