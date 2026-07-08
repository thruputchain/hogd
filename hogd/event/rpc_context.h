#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/thread_checked_mutex.h"
#include "hogd/base/types.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/status.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

class EventRPCContext {
public:
  EventRPCContext(std::string_view work_directory)
    : work_directory_(work_directory)
  {}

  ~EventRPCContext()
  {
    while (pending_detached_coroutines_.load(std::memory_order_acquire) > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  // Acquires a reference to the PerThreadARC<EventBlockContext> for the given
  // block id.
  // Returns nullptr if the block is not open.
  x23::Async<PerThreadARCRef<EventBlockContext>> get_context(BlockID block_id);

  // Opens a new block.
  // Returns an error if the block already exists.
  x23::Async<EventStatus<void>> open_block(BlockID block_id);

  // Closes a block.
  // Returns the root hash of the block if successful.
  // Returns an error if the block is not open.
  x23::Async<EventStatus<SHA256Hash>> close_block(BlockID block_id,
                                                  BlockHeight block_height);

  // Commits a closed block.
  // Sends events to indexers and GCs the block context.
  // Returns the block height on success.
  x23::Async<EventStatus<BlockHeight>> commit_block(BlockID block_id);

  // Abandons a block (active or already closed but not yet committed).
  // Returns an error if the block_id is not found.
  x23::Async<EventStatus<void>> abandon_block(BlockID block_id);

  // This is normally called by commit_block() or abandon_block().
  // However, if the block is abandoned, it might be called by some caller
  // that called get_context(), if the block is abandoned while an outstanding
  // rpc from the executor is in flight.
  x23::Async<void> gc_context(BlockID block_id);

  // Adds an indexer, which the event server will send events to.
  x23::Async<void> add_indexer(indexer_event_client_t &&indexer)
  {
    x23::CoroGuard cg;
    co_await cg(indexers_lock_);
    indexers_.push_back(std::move(indexer));
    co_await cg.reset_and_return();
  }

  // Best-effort relay of a nondeterministic tx-failure diagnostic to every
  // registered indexer. Bypasses the per-block commit pipeline entirely --
  // the tx will never commit. One offline indexer's send error is logged and
  // dropped so it never blocks the other indexers or errors toward the
  // executor. Awaitable (drives the sends to completion) -- used directly by
  // tests; production uses spawn_tx_failure_relay to fire it and forget.
  x23::Async<void> report_tx_failure_to_indexers(SignedTransactionHash tx_hash,
                                                 TxFailureDiagnostic diag);

  // Fire-and-forget wrapper around report_tx_failure_to_indexers: launches it
  // as a detached, drained-at-shutdown coroutine and returns at once, so the
  // executor's report RPC never waits on the indexer relay.
  void spawn_tx_failure_relay(SignedTransactionHash tx_hash,
                              TxFailureDiagnostic diag);

private:
  x23::Async<PerThreadARC<EventBlockContext> *>
  get_context_slow(BlockID block_id);
  void gc_context_internal(
      std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>::iterator
          it);

  x23::Async<EventStatus<void>> wait_for_executors_closed(BlockID block_id);

  struct ContextCache {
    ContextCache() = default;
    ContextCache(ContextCache &&cache) noexcept
      : contexts_(std::move(cache.contexts_))
    {}

#ifdef NDEBUG
    using mutex_type = std::mutex;
#else
    using mutex_type = ThreadCheckedMutex<std::mutex>;
#endif

    mutex_type mtx_;
    std::unordered_map<BlockID, PerThreadARC<EventBlockContext> *> contexts_;
  };

  std::atomic<ssize_t> pending_detached_coroutines_ = 0;
  x23::CoroLock lock_;
  std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>
      event_block_contexts_;
  x23::CoroLock indexers_lock_;
  std::vector<indexer_event_client_t> indexers_;

  PerThread<ContextCache> cache_;
  // In any normal workflow we almost certainly are not seeing more than a
  // couple open blocks at a time.
  constexpr static size_t kMaxCachedBlocks = 5;
  std::vector<ReusableEventBlockContextData> cached_blocks_;
  std::string work_directory_;
};

} // namespace hogd
