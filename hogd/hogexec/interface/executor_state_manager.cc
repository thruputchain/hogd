#include "hogd/hogexec/interface/executor_state_manager.h"

#include <cassert>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "rpc_types.hh"

namespace hogd {

PerThreadARC<ExecutorContext> *
ExecutorStateManager::get_executor_context_slow(BlockID block_id)
{
  PerThreadARC<ExecutorContext> *ctx = nullptr;

  std::scoped_lock g(active_blocks_lock_);

  if (auto it = active_blocks_.find(block_id); it != active_blocks_.end()) {
    ctx = &it->second.context;
  }
  if (!ctx) {
    return nullptr;
  }

  if (!ctx->try_acquire_reference()) [[unlikely]]
    return nullptr;

  // cache in threadlocal so we avoid contention on the active_blocks_ map
  {
    std::scoped_lock g(context_cache_->lock_);
    context_cache_->contexts_.emplace(block_id, ctx);
  }
  return ctx;
}

ExecResult
ExecutorStateManager::open_block(BlockID new_block_id, ExecutionMode mode)
{
  std::scoped_lock g(active_blocks_lock_);

  if (auto it = active_blocks_.find(new_block_id); it != active_blocks_.end()) {
    // Increase the refcount on an existing block
    if (it->second.context.get().mode() != mode) {
      return ExecResult::ExecResultSystemError;
    }
    if (it->second.open_block_count == 0) {
      error("Tried to reopen block {} that is closing", new_block_id);
      return ExecResult::ExecResultSystemError;
    }
    assert(it->second.open_block_count > 0);
    it->second.open_block_count++;
    trace("Increased ref count on existing block {} in mode {}", new_block_id,
          mode);
    return ExecResult::ExecResultOK;
  }

  // Create a new block
  [[maybe_unused]] auto [_, inserted] = active_blocks_.emplace(
      std::piecewise_construct, std::forward_as_tuple(new_block_id),
      std::forward_as_tuple(mode, new_block_id, *db_proxy_,
                            *event_server_proxy_, worker_pool_));
  assert(inserted);
  trace("Opened new block {} in mode {}", new_block_id, mode);
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
  trace("Removing reference to block {}", block_id);
  bool do_gc = false;
  {
    std::scoped_lock g(active_blocks_lock_);
    auto it = active_blocks_.find(block_id);
    if (it == active_blocks_.end()) {
      error("Tried to remove block {} that does not exist", block_id);
      return;
    }
    it->second.open_block_count--;
    if (it->second.open_block_count == 0) {
      if (it->second.context.initiate_shutdown()) {
        do_gc = true;
      }
    }
  }
  if (do_gc) {
    gc_executor_context(block_id);
  }
}

void
ExecutorStateManager::gc_executor_context(BlockID block_id)
{
  trace("Garbage collecting state for block {}", block_id);
  // The blockID must already have had shutdown initiated.
  std::scoped_lock g(active_blocks_lock_);
  auto it = active_blocks_.find(block_id);
  assert(it != active_blocks_.end());

  auto ul = context_cache_.lock_threads();
  context_cache_.for_each(ul, [block_id](ContextCache &cache) {
    std::scoped_lock g(cache.lock_);
    cache.contexts_.erase(block_id);
  });

  active_blocks_.erase(it);
}

} // namespace hogd
