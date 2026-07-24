#include "hogd/event/rpc_context.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <exception>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db_conflict.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/status.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {

namespace {

// Releases a reference acquired by try_acquire_indexer_snapshot(). The last
// release of a retired snapshot destroys it (and with it the ClientRefs of
// any since-removed indexers).
void
release_indexer_snapshot(indexer_snapshot_t *snapshot)
{
  if (snapshot->release_reference()) {
    delete snapshot;
  }
}

// Fan out one failure report to every indexer concurrently (dispatch all,
// then await), like send_events_to_indexers; a per-indexer catch keeps one
// indexer's RPC-layer throw from disturbing the others or the executor-facing
// response. bad_alloc and other non-RPC throws are genuine bugs and still
// terminate (cf. EventServerProxy::emit_event's #2857 containment).
//
// x23::Async is eager (initial_suspend is suspend_never), so the dispatch
// call itself runs the send up to its first suspension and can throw
// synchronously (e.g. XdrBadInput on an over-cap payload) -- guard the
// dispatch loop too, not just the await loop, or one bad indexer would
// abort the fan-out before the others are even dispatched.
x23::Async<void>
relay_tx_failure(std::vector<indexer_event_client_t> &indexers,
                 SignedTransactionHash tx_hash, TxFailureDiagnostic diag)
{
  std::vector<x23::Async<void>> handles;
  handles.reserve(indexers.size());
  for (indexer_event_client_t &indexer : indexers) {
    try {
      handles.push_back(indexer.indexer_event_report_tx_failure(tx_hash, diag));
    } catch (x23::XdrException const &e) {
      warn("dropping tx-failure report to an indexer (dispatch): {}", e.what());
    }
  }
  for (x23::Async<void> &handle : handles) {
    try {
      co_await handle;
    } catch (x23::XdrException const &e) {
      warn("dropping tx-failure report to an indexer: {}", e.what());
    }
  }
  co_return;
}

} // namespace

x23::Async<EventStatus<void>>
EventRPCContext::open_block(BlockID block_id)
{
  info("EventRPCContext::open_block: block_id={} acquiring lock", block_id);
  x23::CoroGuard g;
  co_await g(lock_);
  info("EventRPCContext::open_block: block_id={} lock acquired", block_id);

  // try_emplace, not emplace: emplace may construct the context node before
  // discovering the key exists (libstdc++ does), and destroying that
  // still-kActive context trips the assert in ~EventBlockContext. try_emplace
  // guarantees no construction -- and no move from cached_blocks_.back() --
  // unless the insertion happens.
  bool inserted = false;
  if (!cached_blocks_.empty()) {
    std::tie(std::ignore, inserted) =
        event_block_contexts_.try_emplace(block_id, work_directory_, block_id,
                                          std::move(cached_blocks_.back()));
    if (inserted) {
      cached_blocks_.pop_back();
    }
  }
  else {
    std::tie(std::ignore, inserted) =
        event_block_contexts_.try_emplace(block_id, work_directory_, block_id);
  }

  if (!inserted) {
    co_return EventError{EventErrorCode::kExists};
  }
  co_return EventStatus<void>();
}

x23::Async<EventStatus<void>>
EventRPCContext::wait_for_executors_closed(BlockID block_id)
{
  PerThreadARC<EventBlockContext> *context = nullptr;
  {
    x23::CoroGuard g;
    co_await g(lock_);
    auto it = event_block_contexts_.find(block_id);
    if (it == event_block_contexts_.end()) {
      co_await g.reset_and_return(EventError{EventErrorCode::kBadBlockID});
      panic("unreachable");
    }
    context = &it->second;
    // Acquisition fails when the block is already in ARC shutdown -- e.g.
    // abandoned with executor-held references still outstanding -- which a
    // close request can legitimately race with or follow. Releasing a
    // reference we never acquired would drive the shutdown refcount to zero
    // and destroy the context under the live holders (#3039).
    if (!context->try_acquire_reference()) {
      co_await g.reset_and_return(EventError{EventErrorCode::kBadBlockID});
      panic("unreachable");
    }
  }

  bool success = co_await context->get().await_all_executors_closed();
  bool released = context->release_reference();
  if (released) {
    co_await gc_context(block_id);
  }
  if (!success) {
    co_return EventError{EventErrorCode::kBadBlockID};
  }
  co_return EventStatus<void>{};
}

x23::Async<EventStatus<SHA256Hash>>
EventRPCContext::close_block(BlockID block_id, BlockHeight block_height)
{
  ElapsedTimer timer;
  double last_stage_ts = 0.0;
  auto record_stage = [&](double &stage_out) {
    const double now = timer.elapsed();
    stage_out = now - last_stage_ts;
    last_stage_ts = now;
  };
  double stage_wait_executors_closed = 0.0;
  double stage_wait_lock = 0.0;
  double stage_prepare_context_and_async = 0.0;

  auto status = co_await wait_for_executors_closed(block_id);
  record_stage(stage_wait_executors_closed);
  if (!status.has_value()) {
    co_return status.error();
  }

  // We [std::move(g)] into the detached coroutine below. You cannot move an
  // [x23::CoroGuard], so we wrap it in an [std::unique_ptr<>].
  auto g = std::make_unique<x23::CoroGuard>();
  co_await (*g)(lock_);
  record_stage(stage_wait_lock);

  auto it = event_block_contexts_.find(block_id);
  if (it == event_block_contexts_.end()) {
    co_return EventError{EventErrorCode::kBadBlockID};
  }
  auto &context = it->second;

  // An abandon can land between wait_for_executors_closed() releasing its
  // reference and the lock acquisition above, initiating ARC shutdown while
  // outstanding references keep the entry in the map. Acquire before any use
  // of the (then abandoned) context; the reference is handed to the detached
  // coroutine below, whose final release balances it.
  if (!context.try_acquire_reference()) {
    co_return EventError{EventErrorCode::kBadBlockID};
  }

  context.get().set_block_height(block_height);

  auto file_wait = [](EventBlockContext &context) -> x23::Async<void> {
    auto *pool = x23::ThreadPool::get_thread_pool();
    if (!pool) {
      pool = &default_threadpool();
    }
    co_await pool->bg();
    co_await context.finish_file_write();
    co_return;
  }(context.get());

  SHA256Hash out_hash;
  std::exception_ptr ex;
  try {
    out_hash = context.get().get_root_hash();
  } catch (const std::exception &e) {
    error("close_block: get_root_hash threw: {}; draining file_wait before "
          "rethrowing",
          e.what());
    ex = std::current_exception();
  } catch (...) {
    error("close_block: get_root_hash threw unknown exception; draining "
          "file_wait before rethrowing");
    ex = std::current_exception();
  }
  if (ex) {
    co_await file_wait;
    // The reference acquired above never reaches the detached coroutine on
    // this path. [it] is still valid: the lock has been held throughout.
    if (context.release_reference()) {
      gc_context_internal(it);
    }
    std::rethrow_exception(ex);
  }
  record_stage(stage_prepare_context_and_async);

  pending_detached_coroutines_.fetch_add(1, std::memory_order_acquire);
  [](EventRPCContext &event, std::unique_ptr<x23::CoroGuard> g,
     PerThreadARC<EventBlockContext> *context, BlockID block_id,
     SHA256Hash out_hash, x23::Async<void> file_wait) -> x23::Detached {
    auto *pool = x23::ThreadPool::get_thread_pool();
    if (!pool) {
      pool = &default_threadpool();
    }
    co_await pool->bg();

    // [context]'s reference was acquired by close_block() under [g], which
    // is still held here, so no abandon has been able to run in between.
    context->get().set_closing();
    g = nullptr;

    co_await file_wait;
    co_await context->get().finish_file(out_hash);

    // Scope cg so it unlocks event.lock_ before we decrement the
    // pending-detached counter. ~EventRPCContext spins on that counter
    // and proceeds to destroy lock_ once it reaches zero; if cg were
    // still alive at that point its destructor would CAS into freed
    // memory (event.lock_.last_). Observed as a TSAN data race in tests
    // that stack-allocate EventRPCContext.
    {
      x23::CoroGuard cg;
      co_await cg(event.lock_);

      // Only the gc path needs the map iterator (gc_context_internal
      // erases the entry); released is true only when an abandon raced
      // ahead of this coroutine and initiated shutdown.
      if (context->release_reference()) {
        auto it = event.event_block_contexts_.find(block_id);
        CHECK(it != event.event_block_contexts_.end());
        event.gc_context_internal(it);
      }
    }
    event.pending_detached_coroutines_.fetch_sub(1, std::memory_order_release);
    co_return;
  }(*this, std::move(g), &context, block_id, out_hash, std::move(file_wait));

  // We moved [g] into the detached coroutine above, so be sure not to do any
  // work beyond this point which touches a context!

  info(
      "close_block() stages: wait_executors_closed={:.6f}, wait_lock={:.6f}, "
      "prepare_context_and_async={:.6f}, total={:.6f}, outcome={}, block_id={}",
      stage_wait_executors_closed, stage_wait_lock,
      stage_prepare_context_and_async, timer.elapsed(), "ok", block_id);
  co_return out_hash;
}

x23::Async<EventStatus<BlockHeight>>
EventRPCContext::commit_block(BlockID block_id)
{
  info("EventRPCContext::commit_block: block_id={} acquiring lock", block_id);
  PerThreadARC<EventBlockContext> *context_ptr = nullptr;
  {
    x23::CoroGuard g;
    co_await g(lock_);
    info("EventRPCContext::commit_block: block_id={} lock acquired", block_id);
    auto it = event_block_contexts_.find(block_id);
    if (it == event_block_contexts_.end()) {
      co_return EventError{EventErrorCode::kBadBlockID};
    }
    context_ptr = &it->second;
    if (!context_ptr->get().closing()) {
      co_return EventError{EventErrorCode::kBadBlockID};
    }
    CHECK(context_ptr->try_acquire_reference());
  }

  co_await context_ptr->get().wait_file_done();

  BlockHeight block_height = context_ptr->get().block_height();
  // Send from an acquired snapshot rather than under a lock, so a slow
  // indexer here never blocks add_indexer()/remove_indexer() or the
  // tx-failure relay's slow path. Per-indexer RPC errors are contained
  // inside send_events_to_indexers; anything that still escapes (a genuine
  // bug, e.g. bad_alloc while flattening) is captured so the context
  // release/gc below ALWAYS runs -- rethrowing past it would leak the
  // reference acquired above and pin this BlockID's context forever.
  std::exception_ptr send_ex;
  {
    indexer_snapshot_t *snapshot = try_acquire_indexer_snapshot();
    x23::Defer release{[snapshot] { release_indexer_snapshot(snapshot); }};
    if (!snapshot->get().empty()) {
      try {
        co_await context_ptr->get().send_events_to_indexers(block_height,
                                                            snapshot->get());
      } catch (...) {
        send_ex = std::current_exception();
      }
    }
  }

  info("EventRPCContext::commit_block: block_id={} done, gc", block_id);
  {
    x23::CoroGuard g;
    co_await g(lock_);
    auto it = event_block_contexts_.find(block_id);
    CHECK(it != event_block_contexts_.end());
    bool released = it->second.release_reference();
    if (released || it->second.initiate_shutdown()) {
      gc_context_internal(it);
    }
  }
  if (send_ex) {
    std::rethrow_exception(send_ex);
  }
  co_return block_height;
}

x23::Async<EventStatus<void>>
EventRPCContext::abandon_block(BlockID block_id)
{
  info("EventRPCContext::abandon_block: block_id={} acquiring lock", block_id);

  PerThreadARC<EventBlockContext> *context_ptr = nullptr;
  {
    x23::CoroGuard g;
    co_await g(lock_);
    info("EventRPCContext::abandon_block: block_id={} lock acquired", block_id);

    auto it = event_block_contexts_.find(block_id);
    if (it == event_block_contexts_.end()) {
      co_return EventError{EventErrorCode::kBadBlockID};
    }
    auto &context = it->second;

    if (!context.get().closing()) {
      info("EventRPCContext::abandon_block: block_id={} awaiting abandon",
           block_id);
      co_await context.get().await_abandon();
      context.get().delete_events_file();
      info("EventRPCContext::abandon_block: block_id={} done", block_id);
      if (context.initiate_shutdown()) {
        gc_context_internal(it);
      }
      co_return EventStatus<void>();
    }

    context_ptr = &context;
    CHECK(context_ptr->try_acquire_reference());
  }

  co_await context_ptr->get().wait_file_done();
  context_ptr->get().delete_events_file();

  info("EventRPCContext::abandon_block: block_id={} done (post-close), gc",
       block_id);
  {
    x23::CoroGuard g;
    co_await g(lock_);
    auto it = event_block_contexts_.find(block_id);
    CHECK(it != event_block_contexts_.end());
    bool released = it->second.release_reference();
    if (released || it->second.initiate_shutdown()) {
      gc_context_internal(it);
    }
  }
  co_return EventStatus<void>();
}

x23::Async<void>
EventRPCContext::gc_context(BlockID block_id)
{
  x23::CoroGuard g;
  co_await g(lock_);

  auto it = event_block_contexts_.find(block_id);
  assert(it != event_block_contexts_.end());
  gc_context_internal(it);
}

void
EventRPCContext::gc_context_internal(
    std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>::iterator it)
{
  // the block must already have been shutdown by this point,
  // and should have no outstanding references
  auto thread_ul = cache_.lock_threads();
  cache_.for_each(thread_ul, [&](ContextCache &cache) {
    std::lock_guard g(cache.mtx_);
    cache.contexts_.erase(it->first);
  });

  if (cached_blocks_.size() < kMaxCachedBlocks) {
    cached_blocks_.emplace_back(it->second.get().extract_data());
  }

  event_block_contexts_.erase(it);
}

x23::Async<PerThreadARCRef<EventBlockContext>>
EventRPCContext::get_context(BlockID block_id)
{
  {
    PerThreadARC<EventBlockContext> *ctx = nullptr;
    bool found = false;
    {
      std::scoped_lock g(cache_->mtx_);
      if (auto it = cache_->contexts_.find(block_id);
          it != cache_->contexts_.end()) {
        found = true;
        if (it->second->try_acquire_reference()) {
          ctx = it->second;
        }
        // else ctx = nullptr; we found the context in the cache,
        // but it was already closed/closing, so we do not return a reference to
        // it.
      }
    }
    // Return after releasing cache_->mtx_.
    if (found) {
      co_return PerThreadARCRef<EventBlockContext>(ctx);
    }
  }
  auto *ctx = co_await get_context_slow(block_id);
  co_return PerThreadARCRef<EventBlockContext>(ctx);
}

x23::Async<PerThreadARC<EventBlockContext> *>
EventRPCContext::get_context_slow(BlockID block_id)
{
  x23::CoroGuard g;
  co_await g(lock_);

  auto it = event_block_contexts_.find(block_id);
  if (it == event_block_contexts_.end()) {
    // Avoid building an unbounded unlock->resume recursion chain under heavy
    // lock contention when many coroutines miss in the per-thread cache.
    // Prefer resuming the awaiting caller inline so this hot path avoids an
    // extra thread-pool handoff on return.
    co_await g.reset_and_return_prefer_caller(nullptr);
    panic("unreachable");
  }

  if (!it->second.try_acquire_reference()) {
    co_await g.reset_and_return_prefer_caller(nullptr);
    panic("unreachable");
  }

  // put the context in the threadlocal cache so we do not pay this cost next
  // time.
  {
    std::scoped_lock g(cache_->mtx_);
    cache_->contexts_.emplace(block_id, &it->second);
  }
  co_await g.reset_and_return_prefer_caller(&it->second);
  panic("unreachable");
}

indexer_snapshot_t *
EventRPCContext::try_acquire_indexer_snapshot()
{
  {
    std::scoped_lock g(cache_->mtx_);
    if (indexer_snapshot_t *snapshot = cache_->indexer_snapshot_) {
      // Cannot fail: publish_indexer_snapshot() clears this entry (under
      // this same mutex) before initiating the old snapshot's shutdown, and
      // try_acquire_reference() only fails once shutdown has begun -- so a
      // non-null entry read here always precedes its snapshot's shutdown.
      // CHECK rather than fall through: a failure means that ordering was
      // broken, which is a use-after-free waiting to happen elsewhere, not
      // a condition to paper over with the slow path.
      CHECK(snapshot->try_acquire_reference());
      return snapshot;
    }
  }

  std::scoped_lock g(indexers_mtx_);
  // Cannot fail: the snapshot's shutdown is only initiated after it is
  // unlinked from indexers_snapshot_, under indexers_mtx_.
  CHECK(indexers_snapshot_->try_acquire_reference());
  {
    std::scoped_lock cg(cache_->mtx_);
    cache_->indexer_snapshot_ = indexers_snapshot_;
  }
  return indexers_snapshot_;
}

void
EventRPCContext::publish_indexer_snapshot(
    const std::unique_lock<std::mutex> &indexers_ul)
{
  assert(indexers_ul.owns_lock() && indexers_ul.mutex() == &indexers_mtx_);
  indexer_snapshot_t *old = indexers_snapshot_;
  // An empty indexer list publishes an empty-vector snapshot rather than
  // nullptr, so readers' per-thread caches stay valid (and hot) in the
  // no-indexer case; a cached nullptr always means "not cached".
  std::vector<indexer_event_client_t> clients;
  clients.reserve(indexers_.size());
  for (auto &[id, client] : indexers_) {
    clients.push_back(client);
  }
  indexers_snapshot_ = new indexer_snapshot_t(std::move(clients));

  // Invalidate every thread's cached pointer before retiring [old] (same
  // sweep as gc_context_internal). A reader that acquired a reference under
  // its cache mutex before we sweep its cache is counted by
  // initiate_shutdown() and keeps [old] alive until it releases.
  auto thread_ul = cache_.lock_threads();
  cache_.for_each(thread_ul, [](ContextCache &cache) {
    std::scoped_lock g(cache.mtx_);
    cache.indexer_snapshot_ = nullptr;
  });

  if (old->initiate_shutdown()) {
    delete old;
  }
  // else: destroyed by the last release_indexer_snapshot().
}

uint64_t
EventRPCContext::add_indexer(indexer_event_client_t &&indexer)
{
  std::unique_lock g(indexers_mtx_);
  uint64_t id = next_indexer_id_++;
  indexers_.emplace_back(id, std::move(indexer));
  publish_indexer_snapshot(g);
  return id;
}

bool
EventRPCContext::remove_indexer(uint64_t indexer_id)
{
  std::unique_lock g(indexers_mtx_);
  auto it = std::ranges::find(indexers_, indexer_id,
                              [](const auto &entry) { return entry.first; });
  if (it == indexers_.end()) {
    return false;
  }
  indexers_.erase(it);
  publish_indexer_snapshot(g);
  return true;
}

x23::Async<void>
EventRPCContext::report_tx_failure_to_indexers(SignedTransactionHash tx_hash,
                                               TxFailureDiagnostic diag)
{
  indexer_snapshot_t *snapshot = try_acquire_indexer_snapshot();
  // Defer (not a trailing call): a non-Xdr throw from the fan-out must still
  // release the reference, or a retired snapshot would never be destroyed.
  x23::Defer release{[snapshot] { release_indexer_snapshot(snapshot); }};
  if (snapshot->get().empty()) {
    co_return;
  }
  co_await relay_tx_failure(snapshot->get(), tx_hash, diag);
  co_return;
}

void
EventRPCContext::spawn_tx_failure_relay(SignedTransactionHash tx_hash,
                                        TxFailureDiagnostic diag)
{
  // Fire-and-forget: the executor-facing report RPC hands the relay off to a
  // detached coroutine and returns immediately, so a slow indexer never holds
  // the executor's in-flight-report slot (or the RPC dispatch thread) hostage.
  //
  // The snapshot reference is acquired synchronously here, so the detached
  // coroutine owns everything it touches (the snapshot pins the ClientRefs,
  // which pin their streams; sends run on the process-global coropoll) and
  // never dereferences the EventRPCContext. It therefore needs no
  // pending_detached_coroutines_ tracking -- per-failed-tx traffic on that
  // shared counter is exactly what this path must avoid.
  indexer_snapshot_t *snapshot = try_acquire_indexer_snapshot();
  if (snapshot->get().empty()) {
    // No indexers: release without spawning a coroutine frame at all.
    release_indexer_snapshot(snapshot);
    return;
  }

  // Bounded admission control (#3124). With indexers registered, each spawned
  // relay pins one pending RPC XID per indexer until that indexer replies or
  // its stream fails, and this detached path has no other backpressure. Cap
  // how many may be in flight so a contract that floods best-effort reports
  // at a connected-but-unresponsive indexer cannot make that per-report RPC
  // state grow without bound. Dropping past the cap is acceptable -- the
  // indexer-side tx-failure cache is explicitly best effort.
  //
  // The counter is decremented by the detached relay below through the
  // shared_ptr it owns, so the accounting stays correct even if the relay
  // outlives this EventRPCContext (which deliberately never drains this path).
  if (tx_failure_relays_in_flight_->fetch_add(1, std::memory_order_relaxed) >=
      kMaxInFlightTxFailureRelays) {
    tx_failure_relays_in_flight_->fetch_sub(1, std::memory_order_relaxed);
    warn_every_n_sec(5.0F, "tx-failure relay dropped: in-flight cap reached");
    release_indexer_snapshot(snapshot);
    return;
  }

  // Hand the counter to the relay as a shared_ptr it owns, so it can
  // decrement on completion even after this EventRPCContext is destroyed.
  auto in_flight = tx_failure_relays_in_flight_;
  [](std::shared_ptr<std::atomic<size_t>> in_flight,
     indexer_snapshot_t *snapshot, SignedTransactionHash hash,
     TxFailureDiagnostic d) -> x23::Detached {
    // Runs on any exit (including a non-Xdr throw escaping the fan-out):
    // release the snapshot ref and free the in-flight slot.
    x23::Defer cleanup{[in_flight, snapshot] {
      release_indexer_snapshot(snapshot);
      in_flight->fetch_sub(1, std::memory_order_relaxed);
    }};
    co_await relay_tx_failure(snapshot->get(), hash, d);
    co_return;
  }(in_flight, snapshot, std::move(tx_hash), std::move(diag));
}

} // namespace hogd
