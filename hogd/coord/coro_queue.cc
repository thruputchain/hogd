#include "hogd/coord/coro_queue.h"

namespace hogd {

CoroQueue::CoroQueue(uint32_t max_pending_operations, x23::CoroPoll &cp)
  : max_pending_operations_(max_pending_operations), cp_(cp)
{}

x23::Async<void>
CoroQueue::awake_all_pending_and_release_one()
{
  std::vector<std::coroutine_handle<>> handles;

  x23::CoroGuard g;
  co_await g(lock_);
  assert(running_coroutine_count_ > 0);
  running_coroutine_count_--;

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
CoroQueue::enqueue(std::function<x23::Async<void>()> work,
                   std::atomic_flag &done)
{

  std::vector<x23::Async<void>> results;

  auto work_coro = [this, &work] -> x23::Async<void> {
    co_await cp_.pool_.bg();
    co_await work();
    co_await awake_all_pending_and_release_one();
    co_return;
  };

  // This is the main loop that enqueues work coroutines.
  // This is the _only_ coroutine that gets added to the pending coroutine
  // queue. It keeps adding work_coro instances to the executor (adding
  // the appropriate asyncs to results) until all work is done. It stops adding
  // coroutines when the maximum number of running coroutines is hit (where
  // again, the number of running coroutines refers to the work_coro instances),
  // and suspends itself on the pending coroutine queue.
  auto enqueue_coro = [this, &work_coro, &done, &results] -> x23::Async<void> {
    co_await cp_.pool_.bg();

    while (true) {
      x23::CoroGuard g;
      co_await g(lock_);

      if (done.test()) {
        // This thread doesn't have to wake the other threads,
        // because the end of the work coroutines will wake them up.
        co_return;
      }

      if (running_coroutine_count_ < max_pending_operations_) {
        running_coroutine_count_++;
        results.push_back(work_coro());
        continue;
      }

      auto [async, cb] = x23::make_async<void>();
      pending_coroutines_.push_back(cb);
      g.reset();

      co_await async;
    }
    co_return;
  };

  co_await enqueue_coro();

  for (auto &r : results) {
    co_await r;
  }
}

} // namespace hogd
