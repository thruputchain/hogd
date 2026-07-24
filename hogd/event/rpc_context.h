#pragma once

#include <atomic>
#include <chrono>
#include <memory>
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

// Immutable, refcounted snapshot of the registered indexer clients, plus a
// per-indexer count of best-effort tx-failure relays currently in flight.
//
// [clients] and [tx_failure_in_flight] are INDEX-ALIGNED (same pattern as
// send_events_to_indexers' full_event_log/full_tx_bodies): entry i of each
// belongs to the same indexer. Both are built together in one pass at publish
// and never resized afterward, so the alignment holds for the snapshot's life.
// Keeping [clients] a plain contiguous vector lets commit_block hand it
// straight to send_events_to_indexers with no copy -- the counters, needed
// only by the tx-failure relay path, ride alongside without disturbing it.
//
// The count is PER INDEXER (#3124): each in-flight relay pins one pending RPC
// XID on that indexer's stream until it replies or its stream fails, and the
// detached relay path has no other backpressure. Bounding per indexer --
// rather than globally across all relays -- keeps one connected-but-
// unresponsive indexer from exhausting a shared budget and starving healthy
// indexers of diagnostics. Relaxed throughout: it is a pure admission bound
// and guards no other state.
//
// The counts live in the refcounted snapshot; a relay holds a snapshot
// reference for its whole lifetime, so a count is always alive while any relay
// references it -- even if that relay outlives the EventRPCContext (the
// destructor deliberately does not drain that path). Each is heap-held (via
// unique_ptr) only because std::atomic is not movable and the vector must
// relocate; relays reference it directly, never copying it.
struct IndexerSnapshot {
  std::vector<indexer_event_client_t> clients;
  std::vector<std::unique_ptr<std::atomic<size_t>>> tx_failure_in_flight;
};

// PerThreadARC keeps the acquire/release refcounts thread-local, so the
// per-failed-tx relay path never writes to contended shared state.
using indexer_snapshot_t = PerThreadARC<IndexerSnapshot>;

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
    // Retire the current indexer snapshot. If a fire-and-forget tx-failure
    // relay is still in flight it holds a reference and touches nothing else
    // of this object, so we don't wait: the last release deletes the
    // (heap-allocated) snapshot.
    if (indexers_snapshot_->initiate_shutdown()) {
      delete indexers_snapshot_;
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
  // Returns an id that can later be passed to remove_indexer().
  uint64_t add_indexer(indexer_event_client_t &&indexer);

  // Removes a previously added indexer. Returns false if the id is unknown
  // (e.g. already removed). Relays already in flight hold the prior snapshot
  // and may still complete one last send to the removed indexer; its
  // ClientRefs are destroyed once the last such snapshot drains.
  bool remove_indexer(uint64_t indexer_id);

  // Best-effort relay of a nondeterministic tx-failure diagnostic to every
  // registered indexer. Bypasses the per-block commit pipeline entirely --
  // the tx will never commit. One offline indexer's send error is logged and
  // dropped so it never blocks the other indexers or errors toward the
  // executor. Awaitable (drives the sends to completion) -- used directly by
  // tests; production uses spawn_tx_failure_relay to fire it and forget.
  x23::Async<void> report_tx_failure_to_indexers(SignedTransactionHash tx_hash,
                                                 TxFailureDiagnostic diag);

  // Fire-and-forget wrapper around report_tx_failure_to_indexers: launches it
  // as a detached coroutine and returns at once, so the executor's report RPC
  // never waits on the indexer relay. The relay is NOT drained at shutdown:
  // it owns a reference to the (heap-allocated, refcounted) indexer snapshot
  // and runs on the process-global coropoll, and it must never touch this
  // EventRPCContext -- it may outlive it. Anything added to the relay body
  // must keep that property.
  //
  // Admission is bounded PER INDEXER (kMaxInFlightTxFailureRelaysPerIndexer):
  // a report is dropped for an indexer already at its cap, but still relayed
  // to the others (the indexer-side cache is best effort).
  void spawn_tx_failure_relay(SignedTransactionHash tx_hash,
                              TxFailureDiagnostic diag);

  // Cap on the number of best-effort tx-failure relays in flight to a single
  // indexer at once (#3124). See IndexerSnapshot for why the bound is per
  // indexer rather than global.
  static constexpr size_t kMaxInFlightTxFailureRelaysPerIndexer = 256;

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
      : contexts_(std::move(cache.contexts_)),
        indexer_snapshot_(cache.indexer_snapshot_)
    {}

#ifdef NDEBUG
    using mutex_type = std::mutex;
#else
    using mutex_type = ThreadCheckedMutex<std::mutex>;
#endif

    mutex_type mtx_;
    std::unordered_map<BlockID, PerThreadARC<EventBlockContext> *> contexts_;
    // This thread's cached pointer to the current indexer snapshot
    // (non-owning; references are acquired per use under mtx_). Cleared by
    // publish_indexer_snapshot()'s sweep, exactly like contexts_ entries are
    // cleared by gc_context_internal().
    indexer_snapshot_t *indexer_snapshot_ = nullptr;
  };

  // Fast path for readers of the indexer list: acquire a reference to the
  // current snapshot via this thread's cache, falling back to indexers_mtx_
  // on a miss (mirrors get_context()/get_context_slow()). Never returns
  // nullptr -- with no indexers registered the snapshot is an empty vector,
  // so even that case stays on the per-thread fast path. The caller must
  // pass the result to release_indexer_snapshot() when done.
  indexer_snapshot_t *try_acquire_indexer_snapshot();

  // Rebuilds the published snapshot from indexers_ and retires the previous
  // one (destroyed once its last reference drains). [indexers_ul] is the
  // caller's held lock on indexers_mtx_, passed as a witness so the
  // requirement can't be forgotten (same pattern as SynchronizedMap).
  void
  publish_indexer_snapshot(const std::unique_lock<std::mutex> &indexers_ul);

  std::atomic<ssize_t> pending_detached_coroutines_ = 0;
  x23::CoroLock lock_;
  std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>
      event_block_contexts_;
  // Guards the authoritative indexer list and snapshot publication (writers
  // plus the snapshot slow path). Plain mutex: critical sections are short
  // and never suspend, and in particular no network send happens under it.
  std::mutex indexers_mtx_;
  uint64_t next_indexer_id_ = 0;
  std::vector<std::pair<uint64_t, indexer_event_client_t>> indexers_;
  // Currently published snapshot; owned here until publish_indexer_snapshot()
  // retires it (then owned by whichever reader releases the last reference).
  // Never null: an empty-vector snapshot stands in for "no indexers", so the
  // per-thread fast path works identically in the empty case (a cached
  // nullptr always means "not cached", never "empty").
  indexer_snapshot_t *indexers_snapshot_ =
      new indexer_snapshot_t(IndexerSnapshot{});

  PerThread<ContextCache> cache_;
  // In any normal workflow we almost certainly are not seeing more than a
  // couple open blocks at a time.
  constexpr static size_t kMaxCachedBlocks = 5;
  std::vector<ReusableEventBlockContextData> cached_blocks_;
  std::string work_directory_;
};

} // namespace hogd
