#pragma once

#include <cassert>
#include <functional>
#include <vector>

#include "hogd/base/types.h"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/event.h"
#include "x23/refcnt.h"

namespace hogd {

class CoroQueue {
public:
  CoroQueue(uint32_t max_pending_operations, x23::CoroPoll &cp);

  // Enqueues work to be executed with a maximum number of concurrent
  // operations.
  //
  // This method manages a pool of coroutines that execute the provided work
  // function. It ensures that no more than max_pending_operations_ coroutines
  // are running at once. but will create as many copies of work() as possible
  // up to that limit. When the limit is reached, additional work requests are
  // queued until capacity becomes available.
  //
  // work should be some function that returns an Async<void>, representing the
  // work to be done. Many copies of the work function will be executed. The
  // done flag signals when the work threads are done. It is the responsibility
  // of the caller to ensure that the done flag is set when all work finishes;
  // otherwise, enqueue will block forever.
  //
  // Returns true if the work actually runs, false if it fails to launch
  // (such as if the queue is shutting down).
  x23::Async<bool> enqueue(std::function<x23::Async<void>()> work);

  ~CoroQueue()
  {
    signal_shutdown()();
    dtor_.destroy_wait();
    assert(pending_coroutines_.empty());
    assert(running_coroutine_count_ == 0);
  }

private:
  const uint32_t max_pending_operations_;
  x23::CoroPoll &cp_;

  x23::Async<void> signal_shutdown()
  {
    x23::CoroGuard g;
    co_await g(lock_);
    shutting_down_ = true;
  }

  x23::Async<bool> acquire_reference()
  {
    x23::CoroGuard g;
    co_await g(lock_);
    if (!shutting_down_) {
      dtor_.incref();
      co_return true;
    }
    co_return false;
  }
  x23::Async<void> release_reference()
  {
    x23::CoroGuard g;
    co_await g(lock_);
    dtor_.decref();
  }

  x23::CoroLock lock_;
  bool shutting_down_ = false;
  x23::DestroyInhibit dtor_;

  uint32_t running_coroutine_count_ = 0;
  std::vector<x23::CoroCB<void>> pending_coroutines_;

  x23::Async<void> awake_all_pending(uint16_t release_count);
};

} // namespace hogd
