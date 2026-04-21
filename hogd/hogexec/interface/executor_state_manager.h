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
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/interface/dispatch_pool.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

class ExecutorStateManager {
public:
  ExecutorStateManager(std::unique_ptr<DatabaseProxy> db_proxy,
                       std::unique_ptr<EventServerProxy> event_server_proxy,
                       size_t nthreads)
    : db_proxy_(std::move(db_proxy)),
      event_server_proxy_(std::move(event_server_proxy)),
      worker_pool_(static_cast<int>(nthreads))
  {
    CHECK(nthreads <= std::numeric_limits<int>::max());
  }

  // This acquires a reference to the executor context, if the returned value is
  // not null. The caller must eventually release the reference and, if
  // appropriate, call gc_executor_context().
  x23::MaybeAsync<PerThreadARC<ExecutorContext> *>
  get_executor_context(BlockID block_id);

  // return ExecResultOk if successful, or ExecResultSystemError if
  // the block_id already exists and is of a different mode.
  // These increase the reference count of the executor context
  x23::Async<ExecResult> open_proposal(BlockID new_proposal_id);
  x23::Async<ExecResult> open_validation(BlockID new_validation_id);
  // Decreases the reference count of the executor context.
  x23::Async<ExecResult> remove_block(BlockID block_id);
  x23::Async<ExecResult> finalize_block_shutdown(BlockID block_id);

  // All references to the executor context must be released before calling
  // this.
  x23::Async<void> gc_executor_context(BlockID block_id);

private:
  x23::Async<ExecResult> open_block(BlockID block_id, ExecutionMode mode);

  x23::Async<PerThreadARC<ExecutorContext> *>
  get_executor_context_slow(BlockID block_id);

  void
  stash_snapshot_cache(std::unique_ptr<StorageSnapshotCache> snapshot_cache,
                       x23::CoroGuard const &g);
  std::unique_ptr<StorageSnapshotCache>
  get_snapshot_cache(BlockID block_id, x23::CoroGuard const &g);

  struct ShutdownState {
    std::mutex lock_;
    ExecResult result_ = ExecResult::ExecResultOK;
    x23::CoroFutex<bool> done_{false};

    void set_result(ExecResult result)
    {
      std::scoped_lock lock(lock_);
      result_ = result;
    }

    ExecResult result()
    {
      std::scoped_lock lock(lock_);
      return result_;
    }
  };

  struct ActiveBlock {
    ActiveBlock(ExecutionMode mode, BlockID block_id, DatabaseProxy &db_proxy,
                std::unique_ptr<StorageSnapshotCache> snapshot_cache,
                EventServerProxy &event_server_proxy, DispatchPool &worker_pool)
      : context(mode, block_id, db_proxy, std::move(snapshot_cache),
                event_server_proxy, worker_pool),
        shutdown_state(std::make_shared<ShutdownState>())
    {}
    PerThreadARC<ExecutorContext> context;
    int open_block_count{1};
    std::shared_ptr<ShutdownState> shutdown_state;
  };

  x23::CoroLock active_blocks_lock_;

  std::unordered_map<BlockID, ActiveBlock> active_blocks_;
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
  DispatchPool worker_pool_;
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
