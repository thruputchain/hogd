// worker_pool.h - a thread pool abstraction
//
// submit() is not optimized/intended for very short jobs, but you can use
// for_each_parallel() with a large enough width instead.

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <iterator>
#include <mutex>
#include <new>
#include <queue>
#include <ranges>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>
#include <version>

#include "hogd/base/bits.h"
#include "hogd/base/cleanup.h"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/unique_function.h"

#ifdef __APPLE__
#include "hogd/base/large_stack_thread.h"
#else
#include <thread>
#endif

namespace hogd {

#ifdef __APPLE__
// On macOS, use LargeStackThread to set a larger stack size for WASM
// compilation
using WorkerThread = LargeStackThread;
#else
// On other platforms, use standard jthread
using WorkerThread = std::jthread;
#endif

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
    CHECK_GT(n, 0);
    try {
      workers_.reserve(n);
      for (int i = 0; i < n; ++i) {
        workers_.emplace_back(
            [this](const std::stop_token &stoken) { worker_loop(stoken); });
      }
    } catch (...) {
      request_workers_stop();
      throw;
    }
  }
  ~WorkerPool()
  {
    assert(jobs_.empty());
    request_workers_stop();
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
    pending_jobs_.fetch_add(1, std::memory_order_relaxed);
    jobs_.emplace(
        [p = std::move(promise), fn = std::forward<Func>(func)] mutable {
          // handle void return values
          if constexpr (std::is_void_v<Ret>) {
            try {
              fn();
            } catch (...) {
              warn("exception in worker_pool::submit(); rethrowing");
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
              warn("exception in worker_pool::submit(); rethrowing");
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

  // submit_detached enqueues a fire-and-forget job with no future/promise
  // overhead. The caller must not need the return value or exception
  // propagation — if the lambda throws, it propagates to std::terminate().
  template<typename Func> requires std::is_invocable_r_v<void, Func>
  void submit_detached(Func &&func)
  {
    std::scoped_lock g(lock_);
    pending_jobs_.fetch_add(1, std::memory_order_relaxed);
    jobs_.emplace(std::forward<Func>(func));
    if (active_workers_ < target_workers_)
      cv_.notify_one();
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
    completions.reserve(divide_up<size_t>(r.size(), width));
    // Drain all in-flight futures on scope exit so workers never outlive
    // the locals they capture by reference (r, f, proj).
    Defer drain{[&completions] {
      for (auto &f : completions)
        f.wait();
    }};
    std::exception_ptr eptr;
    // The chunk subranges captured in submit() lambdas contain
    // transform_view iterators whose _M_parent points into |range|.
    // |range| must therefore outlive the try block, because worker threads
    // dereference those iterators while we wait on the futures below.
#ifdef __cpp_lib_ranges_chunk
    auto range = r | std::views::chunk(width);
#endif
    try {
#ifdef __cpp_lib_ranges_chunk
      std::ranges::for_each(range, [this, &f, proj, &completions](
                                       std::ranges::viewable_range auto &&r) {
        completions.emplace_back(
            submit([proj, &f, r = std::forward<decltype(r)>(r)] {
              for (auto &&v : r) {
                f(std::invoke(proj, v));
              }
            }));
      });
#else  // defined(__cpp_lib_ranges_chunk)
      // LLVM doesn't support std::views::chunk yet.
      for (size_t i = 0; i < r.size(); i += width) {
        size_t end = std::min<size_t>(i + width, r.size());

        completions.emplace_back(submit([proj, &f, i, end, &r]() {
          for (size_t j = i; j < end; ++j) {
            f(std::invoke(proj, r[j]));
          }
        }));
      }
#endif // defined(__cpp_lib_ranges_chunk)
    } catch (std::bad_alloc &e) {
      eptr = std::current_exception();
      error("Failed to allocate memory in worker_pool::for_each_parallel: {}",
            e.what());
    }

    drain.reset();
    for (std::future<void> &f : completions) {
      // Rethrows the exception, if it exists.
      f.get();
    }
    if (eptr) [[unlikely]] {
      std::rethrow_exception(eptr);
    }
  }

  // JobScope collects the futures for jobs submitted to a WorkerPool and
  // guarantees every submitted job is joined before the scope is destroyed.
  //
  // This makes it safe for submitted jobs to capture locals (including
  // stack-local objects) by reference: the destructor drains all in-flight
  // jobs on *every* exit path -- normal fall-through, an early return, or an
  // exception unwinding through the scope, including a std::bad_alloc thrown
  // by submit() itself partway through a submit loop. A worker therefore can
  // never dereference a captured local after that local has left scope.
  //
  // It is the reusable form of the scope-exit drain in for_each_parallel();
  // prefer it over a hand-rolled vector<future<void>> + wait loop, which
  // leaves the captured locals exposed if an exception (e.g. an allocation
  // failure in submit()) escapes before the wait loop runs.
  //
  // Typical use:
  //
  //   WorkerPool::JobScope scope(pool);
  //   scope.reserve(chunks.size());
  //   for (const ChunkRange &chunk : chunks)
  //     scope.submit([&trie, &hashes, chunk] { ... });
  //   scope.wait();   // join all jobs, rethrowing the first worker exception
  //
  // Calling wait() is optional but recommended: without it, worker exceptions
  // are swallowed by the destructor drain (there is no path left to surface
  // them). With it, the first worker exception is rethrown after every job has
  // been joined.
  class JobScope {
  public:
    explicit JobScope(WorkerPool &pool = WorkerPool::get_default_pool())
      : pool_(pool)
    {}

    // Draining on destruction is the whole point of this type, so it must run
    // even while unwinding from an exception.
    ~JobScope() { drain(); }

    JobScope(const JobScope &) = delete;
    JobScope &operator=(const JobScope &) = delete;
    JobScope(JobScope &&) = delete;
    JobScope &operator=(JobScope &&) = delete;

    // reserve pre-sizes the future vector so that submit() in a loop does not
    // reallocate. A reallocation could throw after jobs are already queued;
    // the drain still covers that, but reserving avoids the churn.
    void reserve(size_t n) { futures_.reserve(n); }

    // submit enqueues a void-returning job on the pool and retains its future
    // so the scope can join it. If the pool's submit() throws (e.g.
    // std::bad_alloc while building the job), the exception propagates and any
    // already-submitted jobs are drained when the scope unwinds.
    template<typename Func>
    requires std::is_void_v<std::invoke_result_t<std::decay_t<Func>>>
    void submit(Func &&func)
    {
      // Grow the vector *before* queuing the job so the push_back below cannot
      // allocate. Otherwise a reallocation failure would throw after the job
      // is already queued, dropping its future -- the scope could then never
      // join that job, defeating the whole guarantee in exactly the
      // allocation-failure case this type exists to handle. Moving a
      // std::future is noexcept, so once capacity exists the push_back cannot
      // throw.
      if (futures_.size() == futures_.capacity())
        futures_.reserve(std::max<size_t>(1, futures_.capacity() * 2));
      futures_.push_back(pool_.submit(std::forward<Func>(func)));
    }

    // wait joins every submitted job and rethrows the first exception raised
    // by a worker. All jobs are joined even if one throws, so no worker
    // outlives the scope. After wait() returns (or throws) the futures are
    // satisfied and the destructor drain becomes a no-op.
    void wait()
    {
      std::exception_ptr eptr;
      for (std::future<void> &f : futures_) {
        if (!f.valid())
          continue;
        try {
          // Rethrows the worker's exception, if any.
          f.get();
        } catch (...) {
          if (!eptr)
            eptr = std::current_exception();
        }
      }
      if (eptr) [[unlikely]]
        std::rethrow_exception(eptr);
    }

  private:
    void drain() noexcept
    {
      for (std::future<void> &f : futures_)
        if (f.valid())
          f.wait();
    }

    WorkerPool &pool_;
    std::vector<std::future<void>> futures_;
  };

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

  size_t pending_jobs() const
  {
    return pending_jobs_.load(std::memory_order_relaxed);
  }

private:
  std::mutex lock_;
  std::condition_variable cv_;
  std::queue<UniqueFunction<void()>> jobs_;
  int target_workers_;
  int active_workers_;
  bool stopping_{false};
  std::vector<WorkerThread> workers_;
  std::atomic<size_t> pending_jobs_{0};

  void request_workers_stop()
  {
    // Wake all workers before requesting jthread stop. The worker loop does not
    // wait on the stop token, so this also prevents constructor failure from
    // leaving already-started workers blocked during member unwinding.
    {
      std::scoped_lock g(lock_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (auto &worker : workers_) {
      worker.request_stop();
    }
  }

  // worker_loop is the main loop run by each worker thread.
  void worker_loop(const std::stop_token & /*stoken*/)
  {
    while (true) {
      UniqueFunction<void()> fn;
      {
        std::unique_lock lk(lock_);
        active_workers_--;
        cv_.wait(lk, [this] {
          return stopping_ ||
                 (!jobs_.empty() && active_workers_ < target_workers_);
        });
        if (unlikely(stopping_))
          return;
        assert(!jobs_.empty());
        fn = std::move(jobs_.front());
        jobs_.pop();
        active_workers_++;
      }

      fn();
      pending_jobs_.fetch_sub(1, std::memory_order_relaxed);
    }
  }
};

} // namespace hogd
