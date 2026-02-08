#include "hogd/event/rpc_context.h"

#include <cassert>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/status.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/debug.h"
#include "x23/fastclock.h"
#include "x23/threadpool.h"

namespace hogd {

x23::Async<EventStatus<void>>
EventRPCContext::open_block(BlockID block_id)
{
  x23::CoroGuard g;
  co_await g(lock_);

  auto [_, inserted] =
      event_block_contexts_.emplace(std::piecewise_construct,
                                    std::forward_as_tuple(block_id),
                                    std::forward_as_tuple(work_directory_,
                                                          block_id));
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
  auto status = co_await wait_for_executors_closed(block_id);
  if (!status.has_value()) {
    co_return status.error();
  }

  x23::CoroGuard g;
  co_await g(lock_);

  auto it = event_block_contexts_.find(block_id);
  if (it == event_block_contexts_.end()) {
    co_return EventError{EventErrorCode::kBadBlockID};
  }
  auto &context = it->second;
  auto ts = x23::fastclock::now();

  auto file_wait = [](EventBlockContext &context) -> x23::Async<void> {
    auto *pool = x23::ThreadPool::get_thread_pool();
    if (!pool) {
      pool = &default_threadpool();
    }
    co_await pool->bg();
    co_await context.finish_file_write();
    co_return;
  }(context.get());

  auto indexers_handle =
      context.get().send_events_to_indexers(block_height, indexers_);

  auto out_hash = context.get().get_root_hash();
  auto ts2 = x23::fastclock::now();
  co_await file_wait;
  auto ts3 = x23::fastclock::now();
  co_await indexers_handle;
  auto ts4 = x23::fastclock::now();
  co_await context.get().finish_file(out_hash);
  if (it->second.initiate_shutdown()) {
    gc_context_internal(it, g);
  }
  auto ts5 = x23::fastclock::now();
  info("time to get hash: {} ns, time to finish file: {} ns, time to send "
       "events to indexers: {} ns, time to gc context + rename: {} ns",
       ts2 - ts, ts3 - ts2, ts4 - ts3, ts5 - ts4);
  co_return out_hash;
}

x23::Async<EventStatus<void>>
EventRPCContext::abandon_block(BlockID block_id)
{
  x23::CoroGuard g;
  co_await g(lock_);

  auto it = event_block_contexts_.find(block_id);
  if (it == event_block_contexts_.end()) {
    co_return EventError{EventErrorCode::kBadBlockID};
  }
  auto &context = it->second;
  co_await context.get().await_abandon();

  if (context.initiate_shutdown()) {
    gc_context_internal(it, g);
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
  gc_context_internal(it, g);
}

void
EventRPCContext::gc_context_internal(
    std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>::iterator it,
    x23::CoroGuard const & /*unused*/)
{
  // the block must already have been shutdown by this point,
  // and should have no outstanding references
  auto thread_ul = cache_.lock_threads();
  cache_.for_each(thread_ul, [&](ContextCache &cache) {
    std::lock_guard g(cache.mtx_);
    cache.contexts_.erase(it->first);
  });

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
    co_return nullptr;
  }

  if (!it->second.try_acquire_reference()) {
    co_return nullptr;
  }

  // put the context in the threadlocal cache so we do not pay this cost next
  // time.
  {
    std::scoped_lock g(cache_->mtx_);
    cache_->contexts_.emplace(block_id, &it->second);
  }
  co_return &it->second;
}

} // namespace hogd
