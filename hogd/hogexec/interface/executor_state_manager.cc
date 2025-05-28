#include "hogd/hogexec/interface/executor_state_manager.h"

#include <cassert>
#include <memory>

#include "hogd/hogexec/runtime/execution_mode.h"
#include "rpc_types.hh"

namespace hogd {

std::shared_ptr<ExecutorStateManager::ExecutorContext>
ExecutorStateManager::get_executor_context(BlockID block_id, ExecutionMode mode)
{
  auto active_blocks = active_blocks_.take();

  if (auto it = active_blocks->find(block_id); it != active_blocks->end()) {
    auto out = it->second;
    assert(out->mode_ == mode);
    return out;
  }
  return nullptr;
}

void
ExecutorStateManager::open_proposal(BlockID new_proposal_id)
{
  auto active_blocks = active_blocks_.take();

  [[maybe_unused]] auto [_, inserted] = active_blocks->emplace(
      new_proposal_id,
      std::make_unique<ExecutorContext>(ExecutionMode::kProposing,
                                        new_proposal_id, *db_proxy_));
  assert(inserted);
}
void
ExecutorStateManager::open_validation(BlockID new_validation_id)
{
  auto active_blocks = active_blocks_.take();

  [[maybe_unused]] auto [_, inserted] = active_blocks->emplace(
      new_validation_id,
      std::make_unique<ExecutorContext>(ExecutionMode::kValidating,
                                        new_validation_id, *db_proxy_));
  assert(inserted);
}

void
ExecutorStateManager::remove_block(BlockID block_id)
{
  auto active_blocks = active_blocks_.take();
  active_blocks->erase(block_id);
}

} // namespace hogd
