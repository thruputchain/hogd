#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "consensus.hh"
#include "db_conflict.hh"
#include "hogd/base/perthread.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/thread_checked_mutex.h"
#include "hogd/base/types.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/indexer_outbox.h"
#include "hogd/event/status.h"
#include "proposal.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {

namespace detail {
struct IndexerDeliveryState;
} // namespace detail

// Immutable, refcounted snapshot used only by the best-effort tx-failure
// relay. Durable block delivery uses the single serialized V2 lane below.
// [clients] and [tx_failure_in_flight] are index-aligned and immutable for the
// snapshot's life.
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
  explicit EventRPCContext(std::string_view work_directory);

  ~EventRPCContext();

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
                                                  BlockHeight block_height,
                                                  ProposalTxs proposal_txs);

  // Durably publishes a closed block and GCs the block context.
  // Indexer delivery begins only after confirm_committed_block().
  // Returns the block height on success.
  x23::Async<EventStatus<BlockHeight>>
  commit_block(BlockID block_id, ConsensusValue consensus_value);

  // Releases a durably published block for indexer delivery only after the
  // scheduler has also completed its durable commit.
  x23::Async<EventStatus<BlockHeight>>
  confirm_committed_block(ConsensusValue consensus_value);

  x23::Async<EventStatus<void>>
  reconcile_outbox_tip(BlockID block_id, ConsensusValue consensus_value);

  // Abandons a block (active or already closed but not yet committed).
  // Returns an error if the block_id is not found.
  x23::Async<EventStatus<void>> abandon_block(BlockID block_id);

  // This is normally called by commit_block() or abandon_block().
  // However, if the block is abandoned, it might be called by some caller
  // that called get_context(), if the block is abandoned while an outstanding
  // rpc from the executor is in flight.
  x23::Async<void> gc_context(BlockID block_id);

  // Test-only multi-receiver helpers for the best-effort diagnostic relay.
  // Production registration uses register_indexer()'s single startup-only V2
  // slot and serialized block-delivery lane.
  uint64_t add_indexer(indexer_event_client_t &&indexer);

  // Removes a previously added indexer. Returns false if the id is unknown
  // (e.g. already removed). Relays already in flight hold the prior snapshot
  // and may still complete one last send to the removed indexer; its
  // ClientRefs are destroyed once the last such snapshot drains.
  bool remove_indexer(uint64_t indexer_id);

  x23::Async<EventIndexerRegistrationOutcome>
  register_indexer(indexer_event_client_t indexer, const IndexerCardV2 &card);
  EventIndexerRegistrationOutcome probe_indexer(const UPID &generation) const;

  const IndexerOutboxStore &indexer_outbox_store() const
  {
    return indexer_outbox_store_;
  }

  void set_close_reserved_test_hook(std::function<void(BlockID)> hook)
  {
    close_reserved_test_hook_ = std::move(hook);
  }
  void set_delivery_worker_empty_test_hook(std::function<void()> hook);
  void set_delivery_succeeded_test_hook(std::function<void()> hook);

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
  enum class BlockOperation { kClosing, kCommitting, kAbandoning };

  struct CandidatePromotion {
    BlockID block_id;
    IndexerOutboxCandidatePayload candidate;
    IndexerOutboxCommittedPayload committed;
  };

  x23::Async<PerThreadARC<EventBlockContext> *>
  get_context_slow(BlockID block_id);
  void gc_context_internal(
      std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>::iterator
          it);

  x23::Async<bool> reserve_block_operation(BlockID block_id,
                                           BlockOperation operation);
  x23::Async<IndexerOutboxStatus<void>>
  remove_resolved_candidates(BlockHeight committed_height);
  x23::Async<IndexerOutboxStatus<std::optional<CandidatePromotion>>>
  find_canonical_candidate(const ConsensusValue &consensus_value) const;
  std::shared_ptr<void>
  enqueue_indexer_delivery(const IndexerOutboxOrigin &origin,
                           const IndexerOutboxCommittedPayload &payload);
  void start_indexer_delivery_worker(std::shared_ptr<void> worker_owner);
  void replace_diagnostic_indexer(const indexer_event_client_t &indexer);
  x23::Async<void>
  finish_block_operation(BlockID block_id,
                         PerThreadARC<EventBlockContext> *context,
                         bool shutdown);

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

  x23::CoroLock lock_;
  // Serializes origin selection and committed publication. It is released
  // before any indexer network delivery.
  x23::CoroLock outbox_commit_lock_;
  // Serializes startup registration attempts so an exact-generation retry can
  // observe and acknowledge a registration whose reply was lost.
  x23::CoroLock indexer_registration_lock_;
  std::atomic<bool> outbox_reconciled_ = false;
  std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>
      event_block_contexts_;
  std::unordered_map<BlockID, BlockOperation> block_operations_;
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
  std::shared_ptr<detail::IndexerDeliveryState> indexer_delivery_state_;
  // Guarded by outbox_commit_lock_. Records above this scheduler-confirmed
  // height may be durable after crash reconciliation but are not replayable.
  std::optional<BlockHeight> admissible_indexer_tail_;

  PerThread<ContextCache> cache_;
  // In any normal workflow we almost certainly are not seeing more than a
  // couple open blocks at a time.
  constexpr static size_t kMaxCachedBlocks = 5;
  std::vector<ReusableEventBlockContextData> cached_blocks_;
  std::string work_directory_;
  IndexerOutboxStore indexer_outbox_store_;
  std::function<void(BlockID)> close_reserved_test_hook_;
};

} // namespace hogd
