#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "hogd/base/types.h"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/event.h"

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
  x23::Async<void> enqueue(std::function<x23::Async<void>()> work,
                           std::atomic_flag &done);

private:
  const uint32_t max_pending_operations_;
  x23::CoroPoll &cp_;

  x23::CoroLock lock_;

  uint32_t running_coroutine_count_ = 0;
  std::vector<x23::CoroCB<void>> pending_coroutines_;

  x23::Async<void> awake_all_pending_and_release_one();
};

} // namespace hogd
