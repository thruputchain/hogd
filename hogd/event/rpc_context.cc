#include "hogd/event/rpc_context.h"

#include <atomic>
#include <cassert>
#include <exception>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/time.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/status.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/debug.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {

x23::Async<EventStatus<void>>
EventRPCContext::open_block(BlockID block_id)
{
  info("EventRPCContext::open_block: block_id={} acquiring lock", block_id);
  x23::CoroGuard g;
  co_await g(lock_);
  info("EventRPCContext::open_block: block_id={} lock acquired", block_id);

  bool inserted = false;
  if (!cached_blocks_.empty()) {
    std::tie(std::ignore, inserted) = event_block_contexts_.emplace(
        std::piecewise_construct, std::forward_as_tuple(block_id),
        std::forward_as_tuple(work_directory_, block_id,
                              std::move(cached_blocks_.back())));
    cached_blocks_.pop_back();
  }
  else {
    std::tie(std::ignore, inserted) =
        event_block_contexts_.emplace(std::piecewise_construct,
                                      std::forward_as_tuple(block_id),
                                      std::forward_as_tuple(work_directory_,
                                                            block_id));
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
    [[maybe_unused]] auto res = context->try_acquire_reference();
    x23assert(res);
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
    std::rethrow_exception(ex);
  }
  record_stage(stage_prepare_context_and_async);

  pending_detached_coroutines_.fetch_add(1, std::memory_order_acquire);
  [](EventRPCContext &event, std::unique_ptr<x23::CoroGuard> g,
     BlockID block_id, SHA256Hash out_hash,
     x23::Async<void> file_wait) -> x23::Detached {
    auto *pool = x23::ThreadPool::get_thread_pool();
    if (!pool) {
      pool = &default_threadpool();
    }
    co_await pool->bg();

    auto it = event.event_block_contexts_.find(block_id);
    CHECK(it != event.event_block_contexts_.end());
    PerThreadARC<EventBlockContext> *context = &it->second;
    CHECK(context->try_acquire_reference());
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

      it = event.event_block_contexts_.find(block_id);
      CHECK(it != event.event_block_contexts_.end());
      bool released = it->second.release_reference();
      if (released) {
        event.gc_context_internal(it);
      }
    }
    event.pending_detached_coroutines_.fetch_sub(1, std::memory_order_release);
    co_return;
  }(*this, std::move(g), block_id, out_hash, std::move(file_wait));

  // We moved [g] into the detached coroutine above, so be sure not to do any
  // work beyond this point which touches a context or [indexers_]!

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
  {
    x23::CoroGuard cg;
    co_await cg(indexers_lock_);
    co_await context_ptr->get().send_events_to_indexers(block_height,
                                                        indexers_);
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

x23::Async<void>
EventRPCContext::report_tx_failure_to_indexers(SignedTransactionHash tx_hash,
                                               TxFailureDiagnostic diag)
{
  // Snapshot the registered indexer clients under the lock, then release it
  // BEFORE any network send. indexer_event_client_t is a refcounted ClientRef,
  // so each copy keeps its stream alive independently. Holding indexers_lock_
  // across the sends would let one slow or half-open indexer stall
  // commit_block(), which acquires the same lock every block -- and this
  // best-effort diagnostic path must never disturb the commit pipeline.
  std::vector<indexer_event_client_t> indexers_snapshot;
  {
    x23::CoroGuard cg;
    co_await cg(indexers_lock_);
    indexers_snapshot = indexers_;
  }

  // Fan out concurrently (dispatch all, then await), like
  // send_events_to_indexers; a per-indexer catch keeps one indexer's RPC-layer
  // throw from disturbing the others or the executor-facing response.
  // bad_alloc and other non-RPC throws are genuine bugs and still terminate
  // (cf. EventServerProxy::emit_event's #2857 containment).
  std::vector<x23::Async<void>> handles;
  handles.reserve(indexers_snapshot.size());
  for (indexer_event_client_t &indexer : indexers_snapshot) {
    handles.push_back(indexer.indexer_event_report_tx_failure(tx_hash, diag));
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

} // namespace hogd
