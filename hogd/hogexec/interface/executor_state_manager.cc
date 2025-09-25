#include "hogd/hogexec/interface/executor_state_manager.h"

#include <cassert>
#include <memory>

#include "exec_coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "rpc_types.hh"

namespace hogd {

std::shared_ptr<ExecutorContext>
ExecutorStateManager::get_executor_context(
    BlockID block_id) // NOLINT(misc-unused-parameters)
{
  auto active_blocks = active_blocks_.take();

  if (auto it = active_blocks->find(block_id); it != active_blocks->end()) {
    return it->second.context;
  }
  return nullptr;
}

ExecResult
ExecutorStateManager::open_block(BlockID new_block_id, ExecutionMode mode)
{
  auto active_blocks = active_blocks_.take();

  if (auto it = active_blocks->find(new_block_id); it != active_blocks->end()) {
    if (it->second.context->mode() != mode) {
      return ExecResult::ExecResultSystemError;
    }
    it->second.refcount++;
    return ExecResult::ExecResultOK;
  }

  [[maybe_unused]] auto [_, inserted] = active_blocks->emplace(
      new_block_id, ContextEntry{.refcount = 1,
                                 .context = std::make_unique<ExecutorContext>(
                                     mode, new_block_id, *db_proxy_)});
  assert(inserted);
  return ExecResult::ExecResultOK;
}

ExecResult
ExecutorStateManager::open_proposal(BlockID new_proposal_id)
{
  return open_block(new_proposal_id, ExecutionMode::kProposing);
}

ExecResult
ExecutorStateManager::open_validation(BlockID new_validation_id)
{
  return open_block(new_validation_id, ExecutionMode::kValidating);
}

void
ExecutorStateManager::remove_block(BlockID block_id)
{
  auto active_blocks = active_blocks_.take();
  auto it = active_blocks->find(block_id);
  if (it == active_blocks->end()) {
    error("Tried to remove block that does not exist");
    return;
  }
  it->second.refcount--;
  if (it->second.refcount == 0) {
    active_blocks->erase(it);
  }
}

} // namespace hogd
