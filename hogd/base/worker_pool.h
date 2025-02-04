// worker_pool.h - a thread pool abstraction
//
// Not optimized/intended for very short jobs.

#pragma once

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <exception>
#include <future>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/unique_function.h"

namespace hogd {

// WorkerPool multiplexes jobs across a fixed pool of threads.
class WorkerPool {
public:
  // get_default_pool returns the default thread pool. This should be used most
  // of the time, unless you need a custom number of worker threads or to keep
  // jobs separate.
  static WorkerPool &get_default_pool()
  {
    static WorkerPool pool;
    return pool;
  }

  WorkerPool(int n = std::jthread::hardware_concurrency())
    : target_workers_(n), active_workers_(n)
  {
    workers_.reserve(n);
    for (int i = 0; i < n; ++i) {
      workers_.emplace_back(
          [this](const std::stop_token &stoken) { worker_loop(stoken); });
    }
  }
  ~WorkerPool()
  {
    assert(jobs_.empty());
    for (std::jthread &worker : workers_)
      worker.request_stop();
    for (std::jthread &worker : workers_)
      worker.join();
  }

  // disable move and copy.
  WorkerPool(WorkerPool &&p) = delete;
  WorkerPool &operator=(WorkerPool &&p) = delete;
  WorkerPool(const WorkerPool &p) = delete;
  WorkerPool &operator=(const WorkerPool &p) = delete;

  // worker_count returns the maximum number of available workers.
  int worker_count() const { return workers_.size(); }

  // submit enqueues a job and returns a future for when the job completes.
  template<typename Func,
           typename Ret = std::invoke_result_t<std::decay_t<Func>>>
  requires std::is_invocable_r_v<Ret, Func>
  std::future<Ret> submit(Func &&func)
  {
    std::promise<Ret> promise;
    std::future<Ret> future = promise.get_future();

    std::scoped_lock g(lock_);
    jobs_.emplace(
        [p = std::move(promise), fn = std::forward<Func>(func)] mutable {
          // handle void return values
          if constexpr (std::is_void_v<Ret>) {
            try {
              fn();
            } catch (...) {
              p.set_exception(std::current_exception());
              return;
            }
            p.set_value();
          }
          // otherwise handle the general return value case
          else {
            Ret ret;
            try {
              ret = fn();
            } catch (...) {
              p.set_exception(std::current_exception());
              return;
            }
            p.set_value(ret);
          }
        });

    // notify workers of the new job and return.
    if (active_workers_ < target_workers_)
      cv_.notify_one();
    assert(future.valid());
    return future;
  }

  // adjust_workers changes the number of active worker threads. By setting it
  // low or to zero, one can reduce background activity during periods of
  // high-priority work. Setting n to INT_MAX will re-enable all available
  // threads in the pool.
  void adjust_workers(int n)
  {
    n = std::min(n, static_cast<int>(workers_.size()));
    std::scoped_lock g(lock_);
    int old_target = std::exchange(target_workers_, n);
    if (n > old_target)
      cv_.notify_all();
  }

private:
  std::mutex lock_;
  std::condition_variable_any cv_;
  std::queue<UniqueFunction<void()>> jobs_;
  int target_workers_;
  int active_workers_;
  std::vector<std::jthread> workers_;

  // worker_loop is the main loop run by each worker thread.
  void worker_loop(const std::stop_token &stoken)
  {
    while (true) {
      UniqueFunction<void()> fn;
      {
        std::unique_lock lk(lock_);
        active_workers_--;
        cv_.wait(lk, stoken, [this] {
          return !jobs_.empty() && active_workers_ < target_workers_;
        });
        if (unlikely(stoken.stop_requested()))
          return;
        assert(!jobs_.empty());
        fn = std::move(jobs_.front());
        jobs_.pop();
        active_workers_++;
      }

      fn();
    }
  }
};

} // namespace hogd
