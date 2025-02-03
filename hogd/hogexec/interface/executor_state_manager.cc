#include "hogd/hogexec/interface/executor_state_manager.h"

#include <cassert>
#include <map>
#include <memory>
#include <mutex>

#include "hogd/hogexec/interface/executor_state.h"
#include "rpc_types.hh"

namespace hogd {

ExecutorStateManager::ExecutorContext *
ExecutorStateManager::get_executor_context(BlockID block_id, ExecutorMode mode)
{
  // Returns nullptr if blockId has not been initialized
  // No requirement that coordinator notify every executor
  // of every block.
  std::lock_guard lock(mtx_);
  if (shutdown_) {
    return nullptr;
  }

  if (auto it = active_blocks_.find(block_id); it != active_blocks_.end()) {
    auto *ptr = it->second.get();
    if (!ptr->state_.try_acquire_run_lease(mode)) {
      return nullptr;
    }
    return it->second.get();
  }
  return nullptr;
}

void
ExecutorStateManager::await_finish(BlockID block_id)
{
  std::lock_guard lock(mtx_);
  if (auto it = active_blocks_.find(block_id); it != active_blocks_.end()) {
    it->second->state_.await_finish();
    active_blocks_.erase(it);
  }
  // This executor might not have run transactions on this block,
  // and so might not have opened the block on this executor
  // and might not have an entry.
}

void
ExecutorStateManager::await_all_pending_blocks()
{
  std::unique_lock lock(mtx_);
  if (!shutdown_) {
    cv_.wait(lock, [this] { return shutdown_; });
  }

  for (auto &[id, state] : active_blocks_) {
    state->state_.await_finish();
  }
  active_blocks_.clear();
}

void
ExecutorStateManager::initiate_shutdown()
{
  std::lock_guard lock(mtx_);
  shutdown_ = true;
  cv_.notify_one();
}

void
ExecutorStateManager::open_proposal(BlockID new_proposal_id)
{
  std::lock_guard lock(mtx_);
  if (shutdown_) {
    return;
  }

  active_blocks_.emplace(new_proposal_id, std::make_unique<ExecutorContext>(
                                              ExecutorMode::kProposing,
                                              new_proposal_id, db_proxy_));
}
void
ExecutorStateManager::open_validation(BlockID new_validation_id)
{
  std::lock_guard lock(mtx_);
  if (shutdown_) {
    return;
  }

  active_blocks_.emplace(new_validation_id, std::make_unique<ExecutorContext>(
                                                ExecutorMode::kValidating,
                                                new_validation_id, db_proxy_));
}

} // namespace hogd
