#include "hogd/hogexec/interface/executor_state_manager.h"

#include <cassert>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

x23::Async<PerThreadARC<ExecutorContext> *>
ExecutorStateManager::get_executor_context_slow(BlockID block_id)
{
  PerThreadARC<ExecutorContext> *ctx = nullptr;

  x23::CoroGuard g;
  co_await g(active_blocks_lock_);

  if (auto it = active_blocks_.find(block_id); it != active_blocks_.end()) {
    ctx = &it->second.context;
  }
  if (!ctx) {
    co_return nullptr;
  }

  if (!ctx->try_acquire_reference()) [[unlikely]]
    co_return nullptr;

  // cache in threadlocal so we avoid contention on the active_blocks_ map
  {
    std::scoped_lock g(context_cache_->lock_);
    context_cache_->contexts_.emplace(block_id, ctx);
  }
  co_return ctx;
}

x23::Async<ExecResult>
ExecutorStateManager::open_block(BlockID new_block_id, ExecutionMode mode)
{
  x23::CoroGuard g;
  co_await g(active_blocks_lock_);

  if (auto it = active_blocks_.find(new_block_id); it != active_blocks_.end()) {
    // Increase the refcount on an existing block
    if (it->second.context.get().mode() != mode) {
      co_return ExecResult::ExecResultSystemError;
    }
    if (it->second.open_block_count == 0) {
      error("Tried to reopen block {} that is closing", new_block_id);
      co_return ExecResult::ExecResultSystemError;
    }
    assert(it->second.open_block_count > 0);
    it->second.open_block_count++;
    trace("Increased ref count on existing block {} in mode {}", new_block_id,
          mode);
    co_return ExecResult::ExecResultOK;
  }

  // Create a new block
  // GCC 15 has an assert that ptr is not null when dereferencing, which gcc14
  // and clang21 lack.
  assert(db_proxy_);
  assert(event_server_proxy_);

  std::unique_ptr<StorageSnapshotCache> snapshot_cache =
      get_snapshot_cache(new_block_id, g);

  [[maybe_unused]] auto [_, inserted] = active_blocks_.emplace(
      std::piecewise_construct, std::forward_as_tuple(new_block_id),
      std::forward_as_tuple(mode, new_block_id, *db_proxy_,
                            std::move(snapshot_cache), *event_server_proxy_,
                            worker_pool_));
  assert(inserted);

  // Notify event server that we've opened a block
  auto open_res = co_await event_server_proxy_->open_block(new_block_id);
  if (!open_res.has_value()) [[unlikely]] {
    error("Failed to notify event server of block open: {}", new_block_id);
    auto it = active_blocks_.find(new_block_id);
    CHECK(it != active_blocks_.end());
    stash_snapshot_cache(it->second.context.get().extract_snapshot_cache(), g);
    active_blocks_.erase(it);
    co_await g.reset_and_return(ExecResult::ExecResultSystemError);
    panic("unreachable");
  }

  trace("Opened new block {} in mode {}", new_block_id, mode);
  co_return ExecResult::ExecResultOK;
}

x23::Async<ExecResult>
ExecutorStateManager::open_proposal(BlockID new_proposal_id)
{
  return open_block(new_proposal_id, ExecutionMode::kProposing);
}

x23::Async<ExecResult>
ExecutorStateManager::open_validation(BlockID new_validation_id)
{
  return open_block(new_validation_id, ExecutionMode::kValidating);
}

x23::Async<ExecResult>
ExecutorStateManager::remove_block(BlockID block_id)
{
  trace("Removing reference to block {}", block_id);
  ExecResult result = ExecResult::ExecResultOK;
  bool do_gc = false;
  {
    x23::CoroGuard g;
    co_await g(active_blocks_lock_);
    auto it = active_blocks_.find(block_id);
    if (it == active_blocks_.end()) {
      error("Tried to remove block {} that does not exist", block_id);
      co_return ExecResult::ExecResultSystemError;
    }
    it->second.open_block_count--;
    if (it->second.open_block_count < 0) [[unlikely]] {
      error("Tried to remove block {} that has a negative open_block_count",
            block_id);
      co_return ExecResult::ExecResultSystemError;
    }

    if (it->second.open_block_count == 0) {
      if (it->second.context.initiate_shutdown()) {
        do_gc = true;
      }
      auto event_error = co_await it->second.context.get().await_event_return();

      // Notify event server that we're closing our handle to this block
      auto close_res = co_await event_server_proxy_->close_block(block_id);
      if (!close_res.has_value()) [[unlikely]] {
        error("Failed to notify event server of block close: {}", block_id);
        result = ExecResult::ExecResultSystemError;
      }

      if (event_error.has_value()) [[unlikely]] {
        switch (event_error.value()) {
        case SystemErrorCode::Abandon:
          result = ExecResult::ExecResultSystemError;
          break;
        case SystemErrorCode::ValidationFailure:
          result = ExecResult::ExecResultTxValidationFailure;
          break;
        case SystemErrorCode::Unrecoverable:
          error("Unrecoverable: Received system unrecoverable error code from "
                "event server");
          result = ExecResult::ExecResultUnrecoverable;
          break;
        default:
          error("Unrecoverable: Received unhandled system error code from "
                "event server: {}",
                event_error.value());
          result = ExecResult::ExecResultUnrecoverable;
          break;
        }
      }
    }
  }
  if (do_gc) {
    co_await gc_executor_context(block_id);
  }
  co_return result;
}

x23::Async<void>
ExecutorStateManager::gc_executor_context(BlockID block_id)
{
  trace("Garbage collecting state for block {}", block_id);
  // The blockID must already have had shutdown initiated.
  x23::CoroGuard g;
  co_await g(active_blocks_lock_);

  auto it = active_blocks_.find(block_id);
  assert(it != active_blocks_.end());

  // remove_block() waits before close_block() on the normal shutdown path, but
  // in-flight propose/validate RPCs can still spawn detached event callbacks
  // before the final release_reference() hands off cleanup. Drain again here
  // before recycling the ExecutorContext.
  co_await it->second.context.get().await_event_return();

  auto ul = context_cache_.lock_threads();
  context_cache_.for_each(ul, [block_id](ContextCache &cache) {
    std::scoped_lock g(cache.lock_);
    cache.contexts_.erase(block_id);
  });

  stash_snapshot_cache(it->second.context.get().extract_snapshot_cache(), g);
  active_blocks_.erase(it);
}

void
ExecutorStateManager::stash_snapshot_cache(
    std::unique_ptr<StorageSnapshotCache> snapshot_cache,
    x23::CoroGuard const & /*unused*/)
{
  if (free_snapshot_caches_.size() < kMaxCachedSnapshotCaches) {
    free_snapshot_caches_.emplace_back(std::move(snapshot_cache));
  }
}

std::unique_ptr<StorageSnapshotCache>
ExecutorStateManager::get_snapshot_cache(BlockID block_id,
                                         x23::CoroGuard const & /*unused*/)
{
  if (free_snapshot_caches_.empty()) {
    return std::make_unique<StorageSnapshotCache>(block_id, *db_proxy_);
  }
  auto out = std::move(free_snapshot_caches_.back());
  CHECK(out != nullptr);
  out->set_block_id(block_id);
  free_snapshot_caches_.pop_back();
  return out;
}

} // namespace hogd
