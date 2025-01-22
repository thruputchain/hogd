// -*- C++ -*-

#pragma once

#include <coroutine>
#include <new>
#include <queue>
#include <ranges>
#include <stop_token>
#include <thread>
#include <vector>

#include <x23/async.h>
#include <x23/cleanup.h>
#include <x23/err.h>
#include <x23/mvar.h>
#include <x23/vec.h>

namespace x23 {

// Note: this could break binary compatibility, but we don't have that
// across compilers anyway because of how clang deals with
// [[no_unique_address]]:
// https://github.com/llvm/llvm-project/issues/50766
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wunknown-warning-group"
#pragma GCC diagnostic ignored "-Winterference-size"
constexpr size_t hardware_destructive_interference_size =
    std::hardware_destructive_interference_size;
#pragma GCC diagnostic pop

class ThreadPool {
public:
  const size_t nthreads_;

  ThreadPool(size_t nthreads = std::thread::hardware_concurrency());
  ~ThreadPool();

  void go(const is_range_of<std::coroutine_handle<>> auto &rg)
  {
    // Crash if someone tries to use a ThreadPool being destroyed.
    assert(!ss_.stop_requested());
    if (rg.empty())
      return;
    auto qs = qstate_.take();
    bool was_empty = qs->q_.empty();

    // qs->q_.push_range(rg);
    // XXX libstdc++ bug requires the following
    for (std::coroutine_handle<> it : rg)
      qs->q_.push(it);

    if (was_empty) {
      if (size_t n = qs->q_.size(); n > 1)
        qs->not_empty_.notify_all();
      else if (n == 1)
        qs->not_empty_.notify_one();
    }
  }
  void go(std::coroutine_handle<> item)
  {
    go(std::ranges::views::single(item));
  }

  // Make a callback asynchronously in another thread
  template<typename R> void make_callback(CoroCB<R> cb, references<R> auto &&r)
  {
    if (std::coroutine_handle<> h =
            cb.set_result_without_resuming(std::forward<decltype(r)>(r)))
      go(h);
  }
  void make_callback(CoroCB<void> cb)
  {
    if (std::coroutine_handle<> h = cb.set_result_without_resuming())
      go(h);
  }

  // co_await pool.bg() suspends the current coroutine and
  // resumes it as soon as possible in a different thread.
  [[nodiscard]] auto bg()
  {
    struct AwaitBackground {
      ThreadPool &pool;
      bool await_ready() const noexcept { return false; }
      void await_suspend(std::coroutine_handle<> h) const noexcept
      {
        pool.go(h);
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
    std::jthread([this, cb, f = std::forward<F>(f),
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

  // Drain pauses the current thread and doesn't return untill all
  // coroutine handles that were either in the queue or executing at
  // the time drain was invoked have completed.  Do not call this from
  // within a coroutine (which could lead to deadlock if two threads
  // drain simultaneously).  Within a coroutine, use async to invoke
  // drain in another thread (or just spawn a detached thread if you
  // don't need to wait for the action to finish, for instance if you
  // just need to free something after drain completes).
  void drain() noexcept;

  // If you are executing in a thread pool, return the pool.
  // Otherwise return NULL.
  static ThreadPool *get_thread_pool() { return my_thread_pool; }
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
    auto qs = tp->qstate_.take();
    return SuspendCount{std::this_thread::get_id(),
                        qs->tstate_[my_thread_no].count_};
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
    void check() const {
      assert(start_ == get_suspend_count() || !start_.valid());
    }

#else  // NDEBUG
    CrashIfSuspended() noexcept = default;
    explicit CrashIfSuspended(std::nullptr_t) noexcept {}
    void reset() noexcept {}
    void check() const noexcept {}
#endif // NDEBUG
  };

private:
  inline static thread_local ThreadPool *my_thread_pool{};
  inline static thread_local size_t my_thread_no{};

  struct alignas(hardware_destructive_interference_size) ThreadState {
    // 2*count of iterations of the loop.  Even numbers mean the
    // thread is idle, odd numbers means it is executing a coroutine.
    size_t count_{0};
    bool please_wake_{false};
  };

  struct QState {
    std::queue<std::coroutine_handle<>> q_;
    std::condition_variable not_empty_;

    std::unique_ptr<ThreadState[]> tstate_;
    std::condition_variable count_updated_;
  };
  MVar<QState> qstate_;

  std::stop_source ss_;
  std::vector<std::jthread> pool_;

  void worker(size_t threadno);
};

} // namespace x23
