#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include <x23/nodestroy.h>

#include "hogd/base/unique_function.h"

#if defined(__linux__)
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace hogd {

// A single low-priority thread that runs deferred cleanup jobs (e.g. clearing
// a ConcurrentMap's bucket array so it can be recycled) off the thread that
// dropped the object. Jobs run serially, one at a time, so cleanup never fans
// out across cores and competes with latency-sensitive work such as block
// commit.
//
// Process-lifetime and never destroyed, like ConcurrentMap's storage pool, so
// objects owned by statics can still submit jobs during static teardown.
class BackgroundRecycler {
public:
  static BackgroundRecycler &instance()
  {
    static x23::NoDestroy<BackgroundRecycler> recycler;
    return *recycler;
  }

  BackgroundRecycler() : thread_([this] { run(); }) {}
  BackgroundRecycler(const BackgroundRecycler &) = delete;
  BackgroundRecycler &operator=(const BackgroundRecycler &) = delete;

  void submit(UniqueFunction<void()> job)
  {
    {
      std::scoped_lock g(mutex_);
      jobs_.push_back(std::move(job));
      ++submitted_;
    }
    cv_.notify_all();
  }

  // Blocks until every job submitted before this call has finished. For tests
  // that need to observe the effects of recycling.
  void drain()
  {
    std::unique_lock ul(mutex_);
    const size_t target = submitted_;
    cv_.wait(ul, [&] { return completed_ >= target; });
  }

private:
  void run()
  {
#if defined(__linux__)
    // Nice is per-thread on Linux. Best effort: recycling is only ever a
    // throughput concern, so a failure here is harmless.
    (void)setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
#endif
    std::unique_lock ul(mutex_);
    while (true) {
      cv_.wait(ul, [&] { return !jobs_.empty(); });
      {
        // Scoped so the job (and whatever it captured) is destroyed before it
        // is reported done.
        UniqueFunction<void()> job = std::move(jobs_.front());
        jobs_.pop_front();
        ul.unlock();
        job();
      }
      ul.lock();
      ++completed_;
      cv_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<UniqueFunction<void()>> jobs_;
  size_t submitted_ = 0;
  size_t completed_ = 0;
  std::thread thread_;
};

} // namespace hogd
