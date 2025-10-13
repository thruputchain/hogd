// -*- C++ -*-

// This module implements an affinity-aware thread pool for resuming
// coroutines.  Except for tests, you generally only need one
// ThreadPool per program, which can be default-constructed (to create
// one thread per available hardware thread), or given an explicit
// number of software threads to use (generally most useful to reduce
// threads for debugging).
//
// As a general rule AVOID DOING ANYTHING THAT BLOCKS in a ThreadPool
// thread, as the threads in the pool are intended for coroutines that
// suspend themselves.  See ThreadPool::async if you need to run a
// blocking function (e.g., getaddrinfo) from within a coroutine and
// want to suspend the coroutine until a result is available.
//
// A ThreadPool::Job is just a CpuHinted<std::coroutine_handle<>>,
// which can be initialized from a std::coroutine_handle<> but
// remembers the CPU on which it was created.  Hence if you create
// Jobs at the time a coroutine is suspended, they will typically be
// resumed on a CPU that shares the same last-level cache (LLC) as
// where they last executed.
//
// The main method is go, which immediately queues a job or jobs for
// execution:
//
//   void go(std::vector<Job> &&jobs);
//   void go(Job item) { go(std::vector{item}); }
//
// Queuing a batch of jobs is more efficient than queuing jobs one at
// a time, because it allows the ThreadPool to lock a runqueue one and
// enqueue multiple jobs.  If you are making a bunch of callbacks that
// are CoroCBs, the method CoroCB::set_result_without_resuming can be
// useful here, although set_result_without_resuming may not require
// enqueuing if the coroutine is not yet suspended.  Hence the idom is:
//
//   using WorkItem = CpuHinted<CoroCB<Errno>>;
//
//   std::vector<ThreadPool::Job> work;
//   for (WorkItem &wi : /* some source of work */)
//     if (std::coroutine_handle<> h =
//         wi->wi_set_result_without_resuming(Errno{0}))
//       // Returns non-NULL when coroutine needs to be resumed
//       work.emplace_back(h, wi.cpuhint); // preserves hint from WorkItem
//
//   pool.go(std::move(work));
//
// If you only have one CoroCB to run, you can do so with
// ThreadPool::make_callback, but the above idiom will be more
// efficient when you can batch work.
//
// Other notable methods:
//
//   ThreadPool::bg() - you can co_await this to suspend your
//     coroutine immediately and migrade to a different CPU.
//
//   ThreadPool::quiesce() - block until every thread in the pool has
//     returned from whatever coroutine it is invoking.  This allows
//     for RCU-style synchronization with the following pattern:
//        - writers acquire an exclusive lock then quiesce
//        - readers check no exclusive lock is held, then examine
//          shared data safely so long as they don't suspend the
//          coroutine.
//     Note that it is an error to call quiesce() from within a
//     ThreadPool thread, as this would lead to deadlock.
//
// ThreadPool's affinity strategy is as follows.  It keeps one
// runqueue for each LLC on the machine.  For instance, on the
// 64-core, 128-hyperthread AMD EPYC 9554 there are 8 different LLCs,
// so 8 runqueues.  For each job, the runqueues store a coroutine to
// resume, a CPU hint, and the time at which the job was enqueued.
//
// Worker threads float according to the OS scheduler, and a worker
// attempts to service whatever runqueue corresponds to its current
// CPU's LLC, with two exceptions.  First, to avoid starvation, the
// last worker thread servicing a runqueue cannot change to a new
// runqueue unless its old queue is empty.  Second if the current
// CPU's runqueue is empty for a sufficient period, the worker will
// look at two other runqueues and service the older of the two jobs
// at the head of these queues.  The worker checks other runqueues
// after futex-waiting 10, 100, and 1000 microseconds before waiting
// permanently on its own runqueue.  Empirically, a futex wait seems
// to do better than spinning, probably because of background work in
// other threads.
//
// When servicing a runqueue, if there is Job that last ran on the
// same CPU core as the worker, and the job is no more than 5
// microseconds older than the head of the queue, then the worker will
// preferentially resume the coroutine from the same core.  Otherwise,
// it resumes the oldest job in the queue (i.e., the head of the
// queue).
//
// When enqueuing Jobs, ThreadPool::go() will generally add a
// coroutine to the runqueue corresponding to the Job's LLC, but there
// are exceptions for load balancing.  go() processes a batch of jobs
// and sorts them by "affinity queue"--i.e., the runqueue
// corresponding to the LLC of the CPU hint in the Job.  When
// processing a job for which it has not already locked the affinity
// queue, it examines the time of the oldest job on a random runqueue,
// and if that is least 10 microseconds less old than the affinity
// queue, it migrates the job to the other queue.

#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

#include <x23/async.h>
#include <x23/cleanup.h>
#include <x23/config.h>
#include <x23/cpumap.h>
#include <x23/err.h>
#include <x23/mvar.h>
#include <x23/utility.h>
#include <x23/vec.h>

#if HAVE_SCHED_GETCPU
#include <sched.h>
#endif // HAVE_SCHED_GETCPU

namespace x23 {

template<typename T> struct CpuHinted {
  using value_type = T;

  [[no_unique_address]] T value;
  unsigned cpuhint;

  template<std::same_as<T> U = T> requires std::constructible_from<U>
  CpuHinted() noexcept : value{}, cpuhint{}
  {}
  CpuHinted(T v, unsigned h) noexcept : value(std::move(v)), cpuhint(h) {}

  template<std::convertible_to<T> U>
  CpuHinted(const CpuHinted<U> &u) : value(u.value), cpuhint(u.cpuhint)
  {}

  decltype(auto) operator*(this auto &&self) noexcept { return self.value; }
  auto operator->(this auto &&self) noexcept { return &self.value; }

  template<std::same_as<T> U = T> requires requires(const U u) {
    { bool{u} } noexcept;
  }
  explicit operator bool() const noexcept
  {
    return bool{value};
  }

  template<typename... Args>
  decltype(auto) operator()(this auto &&self, Args &&...args)
      requires requires { self.value(std::forward<Args>(args)...); }
  {
    return self.value(std::forward<Args>(args)...);
  }
};

template<typename T> struct CpuHinted<std::atomic<T>> {
  using value_type = std::atomic<T>;

  std::atomic<T> value;
  std::atomic<unsigned> cpuhint;

  template<std::constructible_from<> = T>
  CpuHinted() noexcept : value{}, cpuhint(0)
  {}
  CpuHinted(T v, unsigned h) noexcept : value(std::move(v)), cpuhint(h) {}

  template<std::convertible_to<T> U>
  CpuHinted(const CpuHinted<U> &u) : value(u.value), cpuhint(u.cpuhint)
  {}

  // Since cpuhint is just a hint, it's okay to be wrong when we swap.
  CpuHinted<T> exchange(const CpuHinted<T> &newval)
  {
    return CpuHinted<T>(value.exchange(newval.value),
                        cpuhint.exchange(newval.cpuhint,
                                         std::memory_order_relaxed));
  }
};

class ThreadPool {
public:
  using Job = CpuHinted<std::coroutine_handle<>>;

  const size_t nthreads_;
  const size_t nqueues_;

  ThreadPool(size_t nthreads = nproc());
  ~ThreadPool();

  void go(std::vector<Job> &&jobs);
  void go(Job item) { go(std::vector{item}); }

  // Make a callback asynchronously in another thread.  This is
  // convenient for one callback, but not as efficient as calling go()
  // on a batch of coroutine_handles.  See the discussion in the
  // comment at the top of the file.
  template<typename R>
  void make_callback(CpuHinted<CoroCB<R>> cb, references<R> auto &&r)
  {
    if (std::coroutine_handle<> h =
            cb->set_result_without_resuming(std::forward<decltype(r)>(r)))
      go({h, cb.cpuhint});
  }
  void make_callback(CpuHinted<CoroCB<void>> cb)
  {
    if (std::coroutine_handle<> h = cb->set_result_without_resuming())
      go({h, cb.cpuhint});
  }

  // co_await pool.bg() suspends the current coroutine and resumes it
  // as soon as possible in a different thread.
  [[nodiscard]] auto bg()
  {
    struct AwaitBackground {
      ThreadPool &pool;
      bool await_ready() const noexcept { return false; }
      void await_suspend(std::coroutine_handle<> h) const noexcept
      {
        pool.go({h, cpuid()});
      }
      void await_resume() const noexcept {}
    };
    return AwaitBackground(*this);
  }

  // Queue a callback to run in a thread of the ThreadPool.  Returns
  // immediately.  Since the task will consume one of the tasks in the
  // ThreadPool, it should avoid performing operations that would
  // block the thread for a long time.
  Detached spawn(std::invocable auto cb)
  {
    co_await bg();
    cb();
  }

  // Run a potentially blocking task in a new thread that is not part
  // of the ThreadPool so as to avoid blocking any threads in the
  // pool.  Suspends the current coroutine until the task has
  // completed, then resumes the coroutine in the ThreadPool.  Returns
  // an Async<R> that should be co_awaited, where R is the result of
  // the task.
  template<typename F, typename... Args> requires std::invocable<F, Args...>
  Async<std::invoke_result_t<F, Args...>> async(F &&f, Args &&...args)
  {
    using R = std::invoke_result_t<F, Args...>;
    auto [async, cb] = make_async<R>();
    // Note: since we are detaching the thread, we have to move f and
    // args into the lambda.  Otherwise we could return and they could
    // be destroyed before they are used.  That incurs a second move
    // into the new thread, but that's unavoidable for safety.
    std::jthread([this, cb = CpuHinted(cb, cpuid()), f = std::forward<F>(f),
                  ... args = std::forward<Args>(args)] {
      if constexpr (std::is_void_v<R>) {
        std::invoke(f, args...);
        make_callback(cb);
      }
      else
        make_callback(cb, std::invoke(f, args...));
    }).detach();
    return std::move(async);
  }

  // Return once every thread in the threadpool has been suspended at
  // least once.  It is an error to call this funciton from within the
  // ThreadPool, as that would cause deadlock.
  void quiesce() noexcept;

  // Drain pauses the current thread and doesn't return untill all
  // coroutine handles that were either in the queue or executing at
  // the time drain was invoked have completed.  Do not call this from
  // within the ThreadPool (which could lead to deadlock if two
  // threads drain simultaneously).  Within a coroutine, use async to
  // invoke drain in another thread (or just spawn a detached thread
  // if you don't need to wait for the action to finish, for instance
  // if you just need to free something after drain completes).
  void drain() noexcept;

  // If you are executing in a thread pool, return the pool.
  // Otherwise return NULL.
  static ThreadPool *get_thread_pool() { return my_thread_pool; }

  class SuspendCount {
  public:
    SuspendCount() noexcept = default;
    explicit SuspendCount(std::thread::id tid, size_t count) noexcept
      : thread_(tid), loop_count_(count)
    {}
    bool operator==(const SuspendCount &) const = default;
    bool valid() const { return thread_ != std::thread::id{}; }

  private:
    std::thread::id thread_{};
    size_t loop_count_{};
  };

  static SuspendCount get_suspend_count() noexcept
  {
    ThreadPool *tp = my_thread_pool;
    // If this crashes, you called get_suspend_count in a thread that
    // wasn't part of a thread pool:
    assert(tp);
    return SuspendCount{std::this_thread::get_id(),
                        tp->tstate_[my_thread_no].count_.load(
                            std::memory_order_relaxed)};
  }

  // A CrashIfSuspended will crash your program if the coroutine in
  // which it was allocated has been suspended between when it was
  // constructed and destroyed.  Be sure to declare it
  // [[no_unique_address]] in data structures, as it compiles to empty
  // when NDEBUG is defined.
  struct [[nodiscard]] CrashIfSuspended {
#ifndef NDEBUG
    SuspendCount start_ = get_suspend_count();

    CrashIfSuspended() noexcept = default;
    explicit CrashIfSuspended(std::nullptr_t) noexcept : start_{} {}
    ~CrashIfSuspended() { check(); }

    void reset() noexcept { start_ = SuspendCount{}; }
    void check() const
    {
      assert(start_ == get_suspend_count() || !start_.valid());
    }

#else  // NDEBUG
    constexpr CrashIfSuspended() noexcept = default;
    // Explicit destructor to avoid unused variable warnings
    constexpr ~CrashIfSuspended() {}
    explicit CrashIfSuspended(std::nullptr_t) noexcept {}
    void reset() noexcept {}
    void check() const noexcept {}
#endif // NDEBUG
  };

  std::string show();

private:
  // Timestamp for empty queues
  static constexpr uint64_t kEmpty = std::numeric_limits<uint64_t>::max();

  // Number of threads currently associated with each runqueue.  We
  // assume threads don't migrade that often, so we can pack these
  // integers densely without undue cache interference.
  std::unique_ptr<std::atomic_uint[]> queue_nthreads_;
  unsigned queue_nthreads(int qn)
  {
    return queue_nthreads_[qn].load(std::memory_order_relaxed);
  }

  struct QueueItem {
    Job job;        // Job to run
    uint64_t qtime; // Time job was placed in queue
  };
  struct QueueState {
    std::deque<QueueItem> q;
    bool need_consumed_signal{false};
    std::condition_variable nonempty;
    std::condition_variable consumed;
  };
  using QPtr = MVar<QueueState>::Ptr;
  struct RunQueue : public MVar<QueueState> {
    // qtime of oldest Job at back of queue, or kEmpty if empty.  Kept
    // outside the queue state in its own cache line so it can be
    // checked approximately without acquiring the mutex.
    std::atomic_uint64_t qtime_{kEmpty};
    uint64_t qtime() const { return qtime_.load(std::memory_order_relaxed); }
    static void set_qtime(const QPtr &ptr, uint64_t newqt)
    {
      auto *rq = static_cast<RunQueue *>(ptr.get_mvar());
      rq->qtime_.store(newqt, std::memory_order_relaxed);
    }
  };

  std::unique_ptr<RunQueue[]> runqueues_;

  // Return which queue a QPtr references, or -1 if none
  int qno(const QPtr &ptr) const
  {
    if (!ptr)
      return -1;
    auto ret = static_cast<RunQueue *>(ptr.get_mvar()) - runqueues_.get();
    assert(size_t(ret) < nqueues_);
    return ret;
  }

  // Lock queue number n if it isn't already locked.  First unlocks
  // the previously locked queue (if any) to avoid deadlock.
  void relock(QPtr &ptr, int n)
  {
    assert(size_t(n) < nqueues_);
    if (qno(ptr) != n) {
      ptr.reset();
      ptr = runqueues_[n].take();
    }
  }

  class Worker {
  public:
    Worker(ThreadPool *pool, size_t threadno) noexcept;
    Worker(Worker &&) noexcept = delete;

    void work();

  private:
    ThreadPool *const pool_;
    const size_t threadno_;
    uint64_t rndseed_;
    int cpu_;
    int bucket_;

    void lock_affinity_queue(QPtr &ptr) noexcept;
    QPtr getq() noexcept;
    std::coroutine_handle<> qpop(QPtr &ptr);
  };

  struct alignas(hardware_destructive_interference_size) ThreadState {
    // 2*count of iterations of the loop.  Even numbers mean the
    // thread is idle, odd numbers means it is executing a coroutine.
    std::atomic_size_t count_{0};
    std::atomic_bool please_wake_{false};
  };
  std::unique_ptr<ThreadState[]> tstate_;

  inline static thread_local ThreadPool *my_thread_pool{};
  inline static thread_local size_t my_thread_no{};

  std::stop_source ss_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<std::jthread> threads_;

  // https://en.wikipedia.org/wiki/Xorshift
  static uint64_t rnd(uint64_t &seed) noexcept
  {
    uint64_t x = seed;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return seed = x;
  }
  // Pick a number in [0,..,skip-1,skip+1,..max)
  static uint64_t rnd_skip(uint64_t &seed, uint64_t max, uint64_t skip)
  {
    uint64_t r = rnd(seed) % (max - 1);
    r += (r >= skip);
    return r;
  }

  void qselect(QPtr *lq, unsigned cpuhint);
};

} // namespace x23
