#pragma once

#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exec_result.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/execution_mode.h"
#include "hogd/hog_exec/interface/executor_context.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

class ExecutorStateManager {
public:
  ExecutorStateManager(std::unique_ptr<DatabaseProxy> db_proxy,
                       std::unique_ptr<EventServerProxy> event_server_proxy)
    : db_proxy_(std::move(db_proxy)),
      event_server_proxy_(std::move(event_server_proxy))
  {}

  // This acquires a reference to the executor context, if the returned value is
  // not null. The caller must eventually release the reference and, if
  // appropriate, call gc_executor_context().
  x23::MaybeAsync<PerThreadARC<ExecutorContext> *>
  get_executor_context(BlockID block_id);

  // return ExecResultOk if successful, or ExecResultSystemError if
  // the block_id already exists and is of a different mode.
  // These increase the reference count of the executor context
  x23::Async<ExecResult> open_proposal(BlockID new_proposal_id,
                                       BlockExecutionContext block_execution);
  x23::Async<ExecResult> open_validation(BlockID new_validation_id,
                                         BlockExecutionContext block_execution);
  // Decreases the reference count of the executor context.  The call that
  // drops it to zero waits for the block's event calls, closes this executor's
  // handle on the event server's block, and tears the context down if no
  // transaction still holds a reference.
  x23::Async<ExecResult> remove_block(BlockID block_id);

  // Called by whoever releases the context's last reference after shutdown
  // began.  Tears the context down, unless remove_block() is still draining or
  // closing it, in which case remove_block() does so when it finishes.  Returns
  // failure if a late event-server error is observed while draining straggler
  // event callbacks (Abandon is treated as working-as-intended and reported as
  // success), or success if the final drain was clean or not this caller's.
  [[nodiscard]] x23::Async<SystemStatus<void>>
  gc_executor_context(BlockID block_id);

private:
  struct ActiveBlock {
    ActiveBlock(ExecutionMode mode, BlockID block_id, DatabaseProxy &db_proxy,
                std::unique_ptr<StorageSnapshotCache> snapshot_cache,
                EventServerProxy &event_server_proxy)
      : context(mode, block_id, db_proxy, std::move(snapshot_cache),
                event_server_proxy)
    {}
    PerThreadARC<ExecutorContext> context;
    int open_block_count{1};
    // Teardown waits for two votes: remove_block() finishing with the closed
    // block, and the context's last reference being released.  Whichever comes
    // second tears the block down, so neither can free the context while the
    // other is still using it.
    int teardown_votes{2};
  };
  using ActiveBlockMap = std::unordered_map<BlockID, ActiveBlock>;

  struct OpenLookup {
    ExecResult result;
    // Null when the lookup settled the open (another reference on an existing
    // block, or a refusal); otherwise the cache for the caller to open with.
    std::unique_ptr<StorageSnapshotCache> snapshot_cache;
  };

  struct RemoveStart {
    ExecResult result;
    // Non-null when this call dropped the last open handle and must drain,
    // close, and vote.
    PerThreadARC<ExecutorContext> *context;
  };

  // Taken by value, not by reference: these are coroutines, and a reference
  // parameter would dangle across the first suspension.
  x23::Async<ExecResult> open_block(BlockID block_id, ExecutionMode mode,
                                    BlockExecutionContext block_execution);

  x23::Async<PerThreadARC<ExecutorContext> *>
  get_executor_context_slow(BlockID block_id);

  // The *_locked helpers each take active_blocks_lock_ for one short critical
  // section and release it through reset_and_return rather than ~CoroGuard
  // (see the comment at the top of executor_state_manager.cc).
  x23::Async<OpenLookup>
  open_block_lookup_locked(BlockID block_id, ExecutionMode mode,
                           BlockExecutionContext block_execution);
  x23::Async<void>
  insert_block_locked(BlockID block_id, ExecutionMode mode,
                      std::unique_ptr<StorageSnapshotCache> snapshot_cache);
  x23::Async<void> stash_snapshot_cache_locked(
      std::unique_ptr<StorageSnapshotCache> snapshot_cache);
  x23::Async<RemoveStart> begin_remove_block_locked(BlockID block_id);
  // Casts one teardown vote.  Returns the context if it was the last vote, in
  // which case the caller must call teardown_block().
  x23::Async<PerThreadARC<ExecutorContext> *>
  cast_teardown_vote_locked(BlockID block_id);
  x23::Async<ActiveBlockMap::node_type>
  extract_block_locked(BlockID block_id,
                       std::unique_ptr<StorageSnapshotCache> snapshot_cache);

  // Drains straggler event calls, drops the context from the per-thread caches
  // and the map, and destroys it.  Only the last teardown voter calls this.
  x23::Async<SystemStatus<void>>
  teardown_block(BlockID block_id, PerThreadARC<ExecutorContext> *context);

  void
  stash_snapshot_cache(std::unique_ptr<StorageSnapshotCache> snapshot_cache,
                       x23::CoroGuard const &g);
  std::unique_ptr<StorageSnapshotCache>
  get_snapshot_cache(BlockID block_id,
                     BlockExecutionContext const &block_execution,
                     x23::CoroGuard const &g);

  // Serializes open_block() calls; see open_block().  Taken before
  // active_blocks_lock_, never while holding it.
  x23::CoroLock open_lock_;

  x23::CoroLock active_blocks_lock_;

  ActiveBlockMap active_blocks_;
  constexpr static size_t kMaxCachedSnapshotCaches = 16;
  std::vector<std::unique_ptr<StorageSnapshotCache>> free_snapshot_caches_;

  struct ContextCache {
    std::mutex lock_;
    std::unordered_map<BlockID, PerThreadARC<ExecutorContext> *> contexts_;

    ContextCache() = default;
    ContextCache(ContextCache &&cache) noexcept
      : contexts_(std::move(cache.contexts_))
    {}
  };

  PerThread<ContextCache> context_cache_;

  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<EventServerProxy> event_server_proxy_;
};

inline x23::MaybeAsync<PerThreadARC<ExecutorContext> *>
ExecutorStateManager::get_executor_context(BlockID block_id)
{
  {
    std::scoped_lock g(context_cache_->lock_);
    if (auto it = context_cache_->contexts_.find(block_id);
        it != context_cache_->contexts_.end()) {
      if (!it->second->try_acquire_reference()) [[unlikely]]
        return {nullptr};
      return {it->second};
    }
  }
  return get_executor_context_slow(block_id);
}

} // namespace hogd
