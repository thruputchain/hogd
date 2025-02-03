#include "hogd/coord/coro_queue.h"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <functional>
#include <vector>

#include "hogd/base/types.h"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/event.h"

namespace hogd {

CoroQueue::CoroQueue(uint32_t max_pending_operations, x23::CoroPoll &cp)
  : max_pending_operations_(max_pending_operations), cp_(cp)
{}

x23::Async<void>
CoroQueue::awake_all_pending(uint16_t release_count)
{
  std::vector<std::coroutine_handle<>> handles;

  x23::CoroGuard g;
  co_await g(lock_);

  assert(running_coroutine_count_ >= release_count);
  running_coroutine_count_ -= release_count;

  while (!pending_coroutines_.empty()) {
    auto back = pending_coroutines_.back();
    if (auto h = back.set_result_without_resuming()) {
      handles.push_back(h);
    }
    pending_coroutines_.pop_back();
  }

  g.reset();

  for (auto h : handles) {
    h.resume();
  }
}

x23::Async<void>
CoroQueue::enqueue(std::function<x23::Async<void>()> work)
{
  std::atomic_flag done = false;
  std::atomic<uint16_t> currently_running_coroutines = max_pending_operations_;

  auto [shutdown_async, shutdown_cb] = x23::make_async<void>();

  auto release_one = [&] (uint16_t release_count) {
    uint16_t currently_running = currently_running_coroutines.fetch_sub(release_count, std::memory_order_relaxed);
    assert(currently_running >= release_count);
    if (currently_running == release_count) {
      // last coro is this one
      shutdown_cb();
    }
    return;
  };

  auto work_coro = [&] -> x23::Detached {
    co_await cp_.pool_.bg();
    co_await work();

    done.test_and_set();
    release_one(1);

    co_return;
  };

  auto enqueue_coro = [&] -> x23::Async<uint16_t> {
    co_await cp_.pool_.bg();

    uint16_t enqueued_count = 0;

    while (enqueued_count < max_pending_operations_) {
      x23::CoroGuard g;
      co_await g(lock_);

      if (done.test(std::memory_order_relaxed)) {
        uint16_t extra = max_pending_operations_ - enqueued_count;
        uint16_t out = enqueued_count;
        enqueued_count = max_pending_operations_;
        release_one(extra);
        co_return out;
      }

      if (running_coroutine_count_ < max_pending_operations_) {
        running_coroutine_count_++;
        enqueued_count++;
        work_coro();
        continue;
      }

      auto [async, cb] = x23::make_async<void>();
      pending_coroutines_.push_back(cb);
      g.reset();

      co_await async;
    }
    co_return enqueued_count;
  };

  uint16_t count = co_await enqueue_coro();
  co_await shutdown_async;
  co_await awake_all_pending(count);
}

} // namespace hogd
