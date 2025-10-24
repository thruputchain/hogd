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
#include "x23/fastclock.h"

namespace hogd {

EventStatus<void>
EventRPCContext::open_block(BlockID block_id)
{
  std::scoped_lock g(mtx_);
  auto [_, inserted] =
      event_block_contexts_.emplace(std::piecewise_construct,
                                    std::forward_as_tuple(block_id),
                                    std::forward_as_tuple(work_directory_,
                                                          block_id));
  if (!inserted) {
    return EventError{EventErrorCode::kExists};
  }
  return {};
}

EventStatus<SHA256Hash>
EventRPCContext::close_block(BlockID block_id)
{
  return SHA256Hash{};

  std::unique_lock ul(mtx_);
  auto it = event_block_contexts_.find(block_id);
  if (it == event_block_contexts_.end()) {
    return EventError{EventErrorCode::kBadBlockID};
  }

  auto &context = it->second;
  auto ts = x23::fastclock::now();

  auto file_wait = [](EventBlockContext &context) -> x23::Async<void> {
    co_await default_threadpool().bg();
    context.finish_file_write();
    co_return;
  }(context.get());

  auto out_hash = context.get().get_root_hash();
  auto ts2 = x23::fastclock::now();
  file_wait();
  auto ts3 = x23::fastclock::now();
  context.get().finish_file(out_hash);
  gc_context_internal(it, ul);
  auto ts4 = x23::fastclock::now();
  info("time to get hash: {} ns, time to finish file: {} ns, time to gc "
       "context + rename: {} ns",
       ts2 - ts, ts3 - ts2, ts4 - ts3);
  return out_hash;
}

EventStatus<void>
EventRPCContext::abandon_block(BlockID block_id)
{
  std::unique_lock ul(mtx_);
  auto it = event_block_contexts_.find(block_id);
  if (it == event_block_contexts_.end()) {
    return EventError{EventErrorCode::kBadBlockID};
  }
  auto &context = it->second;
  if (context.initiate_shutdown()) {
    gc_context_internal(it, ul);
  }
  return {};
}

void
EventRPCContext::gc_context(BlockID block_id)
{
  std::unique_lock ul(mtx_);
  auto it = event_block_contexts_.find(block_id);
  assert(it != event_block_contexts_.end());
  gc_context_internal(it, ul);
}

void
EventRPCContext::gc_context_internal(
    std::unordered_map<BlockID, PerThreadARC<EventBlockContext>>::iterator it,
    std::unique_lock<std::mutex> &ul)
{
  assert(ul.owns_lock());
  assert(ul.mutex() == &mtx_);

  // the block must already have been shutdown by this point,
  // and should have no outstanding references
  auto thread_ul = cache_.lock_threads();
  cache_.for_each(thread_ul, [&](ContextCache &cache) {
    std::lock_guard g(cache.mtx_);
    cache.contexts_.erase(it->first);
  });

  event_block_contexts_.erase(it);
}

PerThreadARC<EventBlockContext> *
EventRPCContext::get_context(BlockID block_id)
{
  {
    std::scoped_lock g(cache_->mtx_);
    if (auto it = cache_->contexts_.find(block_id);
        it != cache_->contexts_.end()) {
      if (!it->second->try_acquire_reference()) {
        return nullptr;
      }
      return it->second;
    }
  }
  return get_context_slow(block_id);
}

PerThreadARC<EventBlockContext> *
EventRPCContext::get_context_slow(BlockID block_id)
{
  std::scoped_lock g(mtx_);

  auto it = event_block_contexts_.find(block_id);
  if (it == event_block_contexts_.end()) {
    return nullptr;
  }

  if (!it->second.try_acquire_reference()) {
    return nullptr;
  }

  // put the context in the threadlocal cache so we do not pay this cost next
  // time.
  {
    std::scoped_lock g(cache_->mtx_);
    cache_->contexts_.emplace(block_id, &it->second);
  }
  return &it->second;
}

} // namespace hogd
