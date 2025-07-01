// -*- C++ -*-

#pragma once

#include <atomic>
#include <coroutine>
#include <deque>
#include <stop_token>
#include <thread>
#include <vector>

#include <x23/async.h>
#include <x23/cleanup.h>
#include <x23/config.h>
#include <x23/err.h>
#include <x23/mvar.h>
#include <x23/uiobuf.h>
#include <x23/vec.h>

#if HAVE_SCHED_GETCPU
#include <sched.h>
#endif // HAVE_SCHED_GETCPU

namespace x23 {

// Return the number of available hardware threads.  Tries to figure
// it out from Cpus_allowed in /proc/self/status, which works on
// modern linux distributions, but then falls back to
// std::thread::hardware_concurrency().
unsigned nproc();

inline unsigned
cpuid() noexcept
{
#if HAVE_SCHED_GETCPU
  return sched_getcpu();
#else  // !HAVE_SCHED_GETCPU
  static const std::hash<std::thread::id> hasher;
  return hasher(std::this_thread::get_id());
#endif // !HAVE_SCHED_GETCPU
}

template<typename T> struct CpuHinted {
  using value_type = T;

  [[no_unique_address]] T value;
  unsigned cpuhint;

  template<std::constructible_from<> = T>
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

struct CpuMap {
  const Vec<int> map_;
  const int nbuckets_;
  int bucket(int n = cpuid()) const noexcept
  {
#if HAVE_SCHED_GETCPU
    if (n < nbuckets_) [[likely]]
      return map_[n];
#endif // HAVE_SCHED_GETCPU
    return map_[n % nbuckets_];
  }
  CpuMap(Vec<int> map) noexcept
    : map_(std::move(map)), nbuckets_(1 + std::ranges::max(map_))
  {}
};

// Return a mapping of CPU numbers to buckets on linux.  For a given
// argument PATH, iterates over the following file for each CPU N:
//
//     /sys/devices/system/cpu/cpuN/PATH
//
// The file is expected to contain a unique string by which CPUs
// should be bucketed.  The following PATHs are useful:
//
//          core - "topology/core_id"
//     L1d cache - "cache/index0/shared_cpu_map"
//     L1i cache - "cache/index1/shared_cpu_map"
//      L2 cache - "cache/index2/shared_cpu_map"
//      L3 cache - "cache/index3/shared_cpu_map"
//
// Note that it doesn't actually parse the shared_cpu_map, files, just
// expects them to be unique per bucket.  Some linux machines have a
// "cache/index[0-3]/id", but this seems to be relatively recent, so
// it's safer to go by the shared_cpu_map file.
//
// Returns a dummy single-bucket mapping if the id files do not exist.
CpuMap make_cpu_map(std::string_view path);

// Calls make_cpu_map for the last level cache, which is generally
// cache/index3/shared_cpu_map or cache/index2/shared_cpu_map.
CpuMap make_llc_map();

class ThreadPool {
public:
  using Job = CpuHinted<std::coroutine_handle<>>;

  const size_t nthreads_;
  const size_t nqueues_;

  ThreadPool(size_t nthreads = nproc());
  ~ThreadPool();

  void go(std::vector<Job> &&jobs);
  void go(Job item) { go(std::vector{item}); }

  // Make a callback asynchronously in another thread
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

  static inline const CpuMap &core_map = [] -> const CpuMap & {
    static const auto map = make_cpu_map("topology/core_id");
    return (map);
  }();
  static inline const CpuMap &llc_map = [] -> const CpuMap & {
    static const auto map = make_llc_map();
    return (map);
  }();

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

  /*
  // If you are executing in a thread pool, return a unique thread
  // identifier in [0, nthreads_].  Otherwise return -1.
  static ptrdiff_t get_thread_no()
  {
    return my_thread_pool ? my_thread_no : -1;
  }

  // A bucket index for the current thread.  Within a ThreadPool, if
  // nbuckets is at least the size of the ThreadPool, each thread will
  // have a unique bucket index.  Outside a ThreadPool, this just
  // hashes the thread id, but the result will always be at least
  // first_non_pool.
  static size_t bucket_index(size_t nbuckets, size_t first_non_pool = 0)
  {
    if (my_thread_pool)
      return my_thread_no % nbuckets;
    constexpr std::hash<std::thread::id> hasher;
    size_t i = hasher(std::this_thread::get_id());
    return first_non_pool + i % (nbuckets - first_non_pool);
  }
  */

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
    assert(ret >= 0 && ret < nqueues_);
    return ret;
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

    void checkcpu() noexcept;
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

  void qselect(QPtr *lq, unsigned cpuhint);
};

} // namespace x23
