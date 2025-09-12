// worker_pool.h - a thread pool abstraction
//
// submit() is not optimized/intended for very short jobs, but you can use
// for_each_parallel() with a large enough width instead.

#pragma once

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <iterator>
#include <mutex>
#include <queue>
#include <ranges>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <version>

#include "hogd/base/bits.h"
#include "hogd/base/compiler.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
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

  WorkerPool(int n = available_parallelism())
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
    // Make teardown slightly faster for large numbers of threads.
    // Without this the destructor signals workers to stop one by one.
    for (auto &worker : workers_) {
      worker.request_stop();
    }
  }

  // disable move and copy.
  WorkerPool(WorkerPool &&p) = delete;
  WorkerPool &operator=(WorkerPool &&p) = delete;
  WorkerPool(const WorkerPool &p) = delete;
  WorkerPool &operator=(const WorkerPool &p) = delete;

  // worker_count returns the maximum number of available workers.
  size_t worker_count() const { return workers_.size(); }

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

  // for_each_parallel divides a range into jobs and runs f on its elements in
  // parallel. The width defines the number of elements per job (i.e.,
  // the batching factor). This function blocks until all jobs finish.
  template<std::ranges::input_range R, class Proj = std::identity,
           std::indirectly_unary_invocable<
               std::projected<std::ranges::iterator_t<R>, Proj>>
               Func>
  void for_each_parallel(R &&r, Func f,
                         size_t width = kDefaultParallelRangeWidth,
                         Proj proj = {})
  {
    std::vector<std::future<void>> completions;
    completions.reserve(divide_up(r.size(), width));
#if defined(__cpp_lib_ranges_chunk)
    std::ranges::for_each(r | std::views::chunk(width),
                          [this, &f, proj,
                           &completions](std::ranges::viewable_range auto &&r) {
                            completions.emplace_back(submit(
                                [proj, &f, r = std::forward<decltype(r)>(r)] {
                                  for (auto v : r) {
                                    f(std::invoke(proj, v));
                                  }
                                }));
                          });
#else  // defined(__cpp_lib_ranges_chunk)
    // LLVM doesn't support std::views::chunk yet.
    for (size_t i = 0; i < r.size(); i += width) {
      size_t end = std::min(i + width, r.size());

      completions.emplace_back(submit([proj, &f, i, end, &r]() {
        for (size_t j = i; j < end; ++j) {
          f(std::invoke(proj, r[j]));
        }
      }));
    }
#endif // defined(__cpp_lib_ranges_chunk)
    for (const std::future<void> &f : completions)
      f.wait();
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
