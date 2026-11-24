#include "hogd/hog_exec/interface/executor_state_manager.h"

#include <cassert>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/interface/executor_context.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

// Every active_blocks_lock_ critical section below is its own coroutine that
// releases the lock through reset_and_return.  Releasing it by letting
// ~CoroGuard run instead resumes the next waiter as a nested call on this
// stack, which under contention nests once per waiter and grows the stack
// without bound.  The sections only touch the map and the snapshot-cache free
// list: RPCs, event drains, and teardown all run with the lock released.

x23::Async<PerThreadARC<ExecutorContext> *>
ExecutorStateManager::get_executor_context_slow(BlockID block_id)
{
  using result_t = PerThreadARC<ExecutorContext> *;
  PerThreadARC<ExecutorContext> *ctx = nullptr;

  x23::CoroGuard g;
  co_await g(active_blocks_lock_);

  if (auto it = active_blocks_.find(block_id); it != active_blocks_.end()) {
    ctx = &it->second.context;
  }
  if (!ctx) {
    co_await g.reset_and_return(result_t{nullptr});
    panic("unreachable");
  }

  if (!ctx->try_acquire_reference()) [[unlikely]] {
    co_await g.reset_and_return(result_t{nullptr});
    panic("unreachable");
  }

  // cache in threadlocal so we avoid contention on the active_blocks_ map
  {
    std::scoped_lock cache_guard(context_cache_->lock_);
    context_cache_->contexts_.emplace(block_id, ctx);
  }
  co_await g.reset_and_return(ctx);
  panic("unreachable");
}

x23::Async<ExecResult>
ExecutorStateManager::open_block(BlockID new_block_id, ExecutionMode mode,
                                 BlockExecutionContext block_execution)
{
  // open_lock_ keeps a second open of the same id from racing this one while
  // the new block is not yet in active_blocks_: it waits, then finds the block
  // and takes another reference, as it would have under active_blocks_lock_.
  // Nothing else takes open_lock_, so finish_block and transactions on other
  // blocks never wait behind the RPCs below.
  x23::CoroGuard open_guard;
  co_await open_guard(open_lock_);

  auto [result, snapshot_cache] =
      co_await open_block_lookup_locked(new_block_id, mode, block_execution);
  if (!snapshot_cache) {
    // Took another reference on an existing block, or refused to.
    co_await open_guard.reset_and_return(result);
    panic("unreachable");
  }

  auto load_db_tip_res = co_await snapshot_cache->load_db_tip();
  if (!load_db_tip_res.has_value()) [[unlikely]] {
    error("Failed to load db tip for block {}: {}", new_block_id,
          load_db_tip_res.error());
    co_await open_guard.reset_and_return(ExecResult::ExecResultSystemError);
    panic("unreachable");
  }

  // Notify event server that we've opened a block.  The block is published in
  // active_blocks_ only after this succeeds, so no transaction can emit
  // events for it before the event server knows about this executor.
  assert(event_server_proxy_);
  auto open_res = co_await event_server_proxy_->open_block(new_block_id);
  if (!open_res.has_value()) [[unlikely]] {
    error("Failed to notify event server of block open: {}", new_block_id);
    co_await stash_snapshot_cache_locked(std::move(snapshot_cache));
    co_await open_guard.reset_and_return(ExecResult::ExecResultSystemError);
    panic("unreachable");
  }

  co_await insert_block_locked(new_block_id, mode, std::move(snapshot_cache));
  trace("Opened new block {} in mode {}", new_block_id, mode);
  co_await open_guard.reset_and_return(ExecResult::ExecResultOK);
  panic("unreachable");
}

x23::Async<ExecutorStateManager::OpenLookup>
ExecutorStateManager::open_block_lookup_locked(
    BlockID block_id, ExecutionMode mode, BlockExecutionContext block_execution)
{
  x23::CoroGuard g;
  co_await g(active_blocks_lock_);

  if (auto it = active_blocks_.find(block_id); it != active_blocks_.end()) {
    // Increase the refcount on an existing block
    if (it->second.context.get().mode() != mode) {
      co_await g.reset_and_return(
          OpenLookup{.result = ExecResult::ExecResultSystemError,
                     .snapshot_cache = nullptr});
      panic("unreachable");
    }
    // Two opens disagreeing about the block's facts is a scheduler bug, not
    // a refcount bump.
    if (auto const &open = it->second.context.get().block_execution();
        open.block_height != block_execution.block_height ||
        open.block_timepoint != block_execution.block_timepoint) {
      error("Tried to reopen block {} with a different execution context "
            "(height {} vs {}, timepoint {} vs {})",
            block_id, open.block_height, block_execution.block_height,
            open.block_timepoint, block_execution.block_timepoint);
      co_await g.reset_and_return(
          OpenLookup{.result = ExecResult::ExecResultSystemError,
                     .snapshot_cache = nullptr});
      panic("unreachable");
    }
    if (it->second.open_block_count <= 0) {
      error("Tried to reopen block {} that is closing", block_id);
      co_await g.reset_and_return(
          OpenLookup{.result = ExecResult::ExecResultSystemError,
                     .snapshot_cache = nullptr});
      panic("unreachable");
    }
    assert(it->second.open_block_count > 0);
    it->second.open_block_count++;
    trace("Increased ref count on existing block {} in mode {}", block_id,
          mode);
    co_await g.reset_and_return(OpenLookup{.result = ExecResult::ExecResultOK,
                                           .snapshot_cache = nullptr});
    panic("unreachable");
  }

  // A new block: hand back a snapshot cache for the caller to fill.
  // GCC 15 has an assert that ptr is not null when dereferencing, which gcc14
  // and clang21 lack.
  assert(db_proxy_);
  co_await g.reset_and_return(
      OpenLookup{.result = ExecResult::ExecResultOK,
                 .snapshot_cache =
                     get_snapshot_cache(block_id, block_execution, g)});
  panic("unreachable");
}

x23::Async<void>
ExecutorStateManager::insert_block_locked(
    BlockID block_id, ExecutionMode mode,
    std::unique_ptr<StorageSnapshotCache> snapshot_cache)
{
  x23::CoroGuard g;
  co_await g(active_blocks_lock_);
  // GCC 15 has an assert that ptr is not null when dereferencing, which gcc14
  // and clang21 lack.
  assert(db_proxy_);
  assert(event_server_proxy_);
  // open_lock_ is held from the lookup that found no block through here, and
  // only open_block() inserts, so the id is still free.
  [[maybe_unused]] auto [_, inserted] =
      active_blocks_.emplace(std::piecewise_construct,
                             std::forward_as_tuple(block_id),
                             std::forward_as_tuple(mode, block_id, *db_proxy_,
                                                   std::move(snapshot_cache),
                                                   *event_server_proxy_));
  assert(inserted);
  co_await g.reset_and_return();
  panic("unreachable");
}

x23::Async<void>
ExecutorStateManager::stash_snapshot_cache_locked(
    std::unique_ptr<StorageSnapshotCache> snapshot_cache)
{
  x23::CoroGuard g;
  co_await g(active_blocks_lock_);
  stash_snapshot_cache(std::move(snapshot_cache), g);
  co_await g.reset_and_return();
  panic("unreachable");
}

x23::Async<ExecResult>
ExecutorStateManager::open_proposal(BlockID new_proposal_id,
                                    BlockExecutionContext block_execution)
{
  return open_block(new_proposal_id, ExecutionMode::kProposing,
                    block_execution);
}

x23::Async<ExecResult>
ExecutorStateManager::open_validation(BlockID new_validation_id,
                                      BlockExecutionContext block_execution)
{
  return open_block(new_validation_id, ExecutionMode::kValidating,
                    block_execution);
}

x23::Async<ExecResult>
ExecutorStateManager::remove_block(BlockID block_id)
{
  trace("Removing reference to block {}", block_id);
  auto [result, context] = co_await begin_remove_block_locked(block_id);
  if (!context) {
    co_return result;
  }

  // This was the last open handle.  The context stays alive without the lock:
  // teardown also needs this call's vote, cast below.
  auto event_error = co_await context->get().await_event_return();

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

  if (auto *last = co_await cast_teardown_vote_locked(block_id)) {
    // No transaction holds the context any more, so teardown runs here before
    // we return: a late event error from a straggler callback can still be
    // folded into the finish_block result.  (teardown_block filters Abandon as
    // WAI; the only remaining code an emit can produce is Unrecoverable.)
    if (auto gc_status = co_await teardown_block(block_id, last);
        !gc_status.has_value()) [[unlikely]] {
      error("Unrecoverable: late event-server error {} during gc of block {}",
            std::to_underlying(gc_status.error().code()), block_id);
      result = ExecResult::ExecResultUnrecoverable;
    }
  }
  co_return result;
}

x23::Async<ExecutorStateManager::RemoveStart>
ExecutorStateManager::begin_remove_block_locked(BlockID block_id)
{
  x23::CoroGuard g;
  co_await g(active_blocks_lock_);
  auto it = active_blocks_.find(block_id);
  if (it == active_blocks_.end()) {
    error("Tried to remove block {} that does not exist", block_id);
    co_await g.reset_and_return(
        RemoveStart{.result = ExecResult::ExecResultSystemError,
                    .context = nullptr});
    panic("unreachable");
  }
  if (it->second.open_block_count <= 0) [[unlikely]] {
    // Validate before mutating: a stray remove on an already-closing block
    // (count 0) must not leave the count at -1, which would defeat the reopen
    // guard in open_block and let a shut-down context be revived.
    error("Tried to remove block {} that has a non-positive open_block_count",
          block_id);
    co_await g.reset_and_return(
        RemoveStart{.result = ExecResult::ExecResultSystemError,
                    .context = nullptr});
    panic("unreachable");
  }
  if (--it->second.open_block_count > 0) {
    co_await g.reset_and_return(
        RemoveStart{.result = ExecResult::ExecResultOK, .context = nullptr});
    panic("unreachable");
  }

  // The last handle: from here no transaction can take a new reference.  If
  // none holds one now, the "references released" vote is already in.
  if (it->second.context.initiate_shutdown()) {
    --it->second.teardown_votes;
  }
  co_await g.reset_and_return(RemoveStart{.result = ExecResult::ExecResultOK,
                                          .context = &it->second.context});
  panic("unreachable");
}

x23::Async<SystemStatus<void>>
ExecutorStateManager::gc_executor_context(BlockID block_id)
{
  // The caller released the context's last reference after shutdown began.
  // Tear down now unless remove_block() is still draining or closing, in which
  // case it tears down when it finishes.
  if (auto *last = co_await cast_teardown_vote_locked(block_id)) {
    co_return co_await teardown_block(block_id, last);
  }
  co_return SystemStatus<void>{};
}

x23::Async<PerThreadARC<ExecutorContext> *>
ExecutorStateManager::cast_teardown_vote_locked(BlockID block_id)
{
  using result_t = PerThreadARC<ExecutorContext> *;
  x23::CoroGuard g;
  co_await g(active_blocks_lock_);
  auto it = active_blocks_.find(block_id);
  CHECK(it != active_blocks_.end());
  CHECK(it->second.teardown_votes > 0);
  result_t last =
      --it->second.teardown_votes == 0 ? &it->second.context : nullptr;
  co_await g.reset_and_return(last);
  panic("unreachable");
}

x23::Async<SystemStatus<void>>
ExecutorStateManager::teardown_block(BlockID block_id,
                                     PerThreadARC<ExecutorContext> *context)
{
  trace("Garbage collecting state for block {}", block_id);
  // Nothing else can reach the context now: shutdown has started, every
  // reference is released, and remove_block() is done with it.  So this all
  // runs without active_blocks_lock_, which the map update at the end takes
  // only briefly.

  // remove_block() waits before close_block() on the normal shutdown path, but
  // in-flight propose/validate RPCs can still spawn detached event callbacks
  // before the final release_reference() hands off cleanup. Drain again here
  // before recycling the ExecutorContext.
  auto event_error = co_await context->get().await_event_return();

  {
    auto ul = context_cache_.lock_threads();
    context_cache_.for_each(ul, [block_id](ContextCache &cache) {
      std::scoped_lock g(cache.lock_);
      cache.contexts_.erase(block_id);
    });
  }

  {
    // Clearing the snapshot cache is proportional to what the block touched,
    // so it happens here rather than under the lock.
    auto block =
        co_await extract_block_locked(block_id,
                                      context->get().extract_snapshot_cache());
    // ~ExecutorContext runs as [block] goes out of scope, also unlocked.
  }

  // Abandon here is working-as-intended: this drain runs after close_block()
  // has been sent, so a straggler emit rejected with BadBlockID is expected.
  // Any other late error is a genuine failure the caller must surface.
  if (!event_error.has_value()) {
    co_return SystemStatus<void>{};
  }
  if (event_error.value() == SystemErrorCode::Abandon) {
    warn("Dropped events for block {}: a straggler emit was rejected with "
         "Abandon after close_block (working-as-intended)",
         block_id);
    co_return SystemStatus<void>{};
  }
  co_return SystemStatus<void>{SystemError{event_error.value()}};
}

x23::Async<ExecutorStateManager::ActiveBlockMap::node_type>
ExecutorStateManager::extract_block_locked(
    BlockID block_id, std::unique_ptr<StorageSnapshotCache> snapshot_cache)
{
  x23::CoroGuard g;
  co_await g(active_blocks_lock_);
  auto it = active_blocks_.find(block_id);
  CHECK(it != active_blocks_.end());
  stash_snapshot_cache(std::move(snapshot_cache), g);
  co_await g.reset_and_return(active_blocks_.extract(it));
  panic("unreachable");
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
ExecutorStateManager::get_snapshot_cache(
    BlockID block_id, BlockExecutionContext const &block_execution,
    x23::CoroGuard const & /*unused*/)
{
  if (free_snapshot_caches_.empty()) {
    return std::make_unique<StorageSnapshotCache>(block_id, block_execution,
                                                  *db_proxy_);
  }
  auto out = std::move(free_snapshot_caches_.back());
  CHECK(out != nullptr);
  out->set_block_id(block_id);
  // A recycled cache still carries the previous block's context.
  out->set_block_execution_context(block_execution);
  free_snapshot_caches_.pop_back();
  return out;
}

} // namespace hogd
