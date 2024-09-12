// -*- C++ -*-

#pragma once

#include <coroutine>
#include <cstdint>
#include <new>
#include <queue>
#include <ranges>
#include <shared_mutex>
#include <stdexcept>
#include <stop_token>
#include <sys/socket.h>
#include <thread>
#include <vector>

#include <sys/epoll.h>

#include <x23/async.h>
#include <x23/cleanup.h>
#include <x23/err.h>
#include <x23/mvar.h>
#include <x23/vec.h>

namespace x23 {

#if __clang__
constexpr size_t hardware_destructive_interference_size = 64;
#else // !__clang__
// Note: this could break binary compatibility, but we don't have that
// across compilers anyway because of how clang deals with
// [[no_unique_address]]:
// https://github.com/llvm/llvm-project/issues/50766
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
constexpr size_t hardware_destructive_interference_size =
    std::hardware_destructive_interference_size;
#pragma GCC diagnostic pop
#endif // !__clang__

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
  // of the ThreadPool so as to avoid blocking any threas in the pool.
  // Suspends the current coroutine until the task has completed, then
  // resumes the coroutine in the ThreadPool.  Returns an Async<R>
  // that should be co_awaited, where R is the result of the task.
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

  class SuspendCount {
  public:
    SuspendCount() noexcept = default;
    explicit SuspendCount(std::thread::id tid, size_t count) noexcept
      : thread_(tid), loop_count_(count)
    {}
    bool operator==(const SuspendCount &) const = default;

  private:
    std::thread::id thread_;
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
  struct CrashIfSuspended {
#ifndef NDEBUG
    SuspendCount start_;
    CrashIfSuspended() : start_(get_suspend_count()) {}
    CrashIfSuspended(const CrashIfSuspended &) = delete;
    ~CrashIfSuspended() { assert(start_ == get_suspend_count()); }
#endif // !NDEBUG
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

// DestroyInhibit is a reference count that waits for all references
// to be released before destroy_wait() returns.
class DestroyInhibit {
public:
  DestroyInhibit() noexcept = default;
  DestroyInhibit(DestroyInhibit &&) = delete;
  ~DestroyInhibit() { assert(refcnt_.load(std::memory_order_relaxed) == 0); }

  void incref() { refcnt_.fetch_add(2, std::memory_order_relaxed); }
  void decref()
  {
    auto c = refcnt_.fetch_sub(2, std::memory_order_release) - 2;
    if (c == 1) {
      refcnt_.notify_all();
      refcnt_.fetch_sub(1, std::memory_order_relaxed);
    }
  }
  void destroy_wait()
  {
    // Here we assume that once there are no more references, incref
    // will no longer be called
    auto c = refcnt_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c > 1) {
      do {
        refcnt_.wait(c, std::memory_order_relaxed);
        c = refcnt_.load(std::memory_order_relaxed);
      } while (c > 1);
      while (refcnt_.load(std::memory_order_relaxed) != 0)
        detail::pause_instruction();
    }
    else
      refcnt_.store(0, std::memory_order_relaxed);
    // Synchronize with all previous decrefs
    refcnt_.load(std::memory_order_acquire);
  }

private:
  // refcount_ = 2 * num_refs + (notification_wanted ? 1 : 0)
  //
  // We jump through some hoops to avoid calling refcnt_.notify_all()
  // after refcnt_ has been destroyed.  (Even though notifying a
  // destroyed atomic would be fine with the underlying futex, it's
  // undefined behavior in C++.)  The strategy is for destroy_wait to
  // set the low bit, and for the last decref (if any are remaining)
  // to clear the low bit after notifying.  After setting the low bit,
  // if destroy_wait finds a non-zero reference count, it waits for
  // refcnt_ to be <= 1, then spins until it reaches 0 before
  // returning.
  std::atomic<unsigned> refcnt_{0};
};

// Refcounted pointer for classes with incref()/decref() methods.
template<typename T> class RefPtr {
public:
  using element_type = T;

  RefPtr(std::nullptr_t) : p_(nullptr) {}
  RefPtr(T &t) : p_(&t) { p_->incref(); }
  RefPtr(const RefPtr &other) : RefPtr(*other) {}
  RefPtr(RefPtr &&other) : p_(std::exchange(other.p_, nullptr)) {}
  ~RefPtr() { clear(); }
  RefPtr &operator=(std::nullptr_t)
  {
    clear();
    return *this;
  }
  RefPtr &operator=(const RefPtr &other)
  {
    if (p_ != other.p_) {
      if (p_)
        p_->decref();
      p_ = other.p_;
      if (p_)
        p_->incref();
    }
    return *this;
  }
  RefPtr &operator=(RefPtr &&other)
  {
    clear();
    p_ = std::exchange(other.p_, nullptr);
    return *this;
  }
  T *get() const { return p_; }
  T &operator*() const { return *p_; }
  T *operator->() const { return p_; }
  void clear()
  {
    if (p_) {
      p_->decref();
      p_ = nullptr;
    }
  }

private:
  T *p_;
};

class CoroPoll : private DestroyInhibit {
public:
  using WorkItem = CoroCB<Errno>;
  ThreadPool &pool_;

  CoroPoll(ThreadPool &pool);
  ~CoroPoll();

  void read_clear(BorrowedFd fd) { clear(fd, false); }

  // Do a read_clear(fd) before a read operation on an fd that might
  // return EAGAIN.  If it does return EAGAIN, then co_await
  // read_wait(fd) to wait for the file descriptor to be readable
  // again.
  Async<Errno> read_wait(BorrowedFd fd)
  {
    auto [async, cb] = make_async<Errno>();
    setcb(fd, false, cb);
    return std::move(async);
  }

  void write_clear(BorrowedFd fd) { clear(fd, true); }

  // Do a write_clear(fd) before a write operation on an fd that might
  // return EAGAIN.  If it does return EAGAIN, then co_await
  // write_wait(fd) to wait for the file descriptor to be writable
  // again.
  Async<Errno> write_wait(BorrowedFd fd)
  {
    auto [async, cb] = make_async<Errno>();
    setcb(fd, true, cb);
    return std::move(async);
  }

  // You must reset a file descriptor before closing it if you have
  // ever called read_clear(), write_clear(), or poison() on it, even
  // if there are not any currently active callbacks.  This is
  // required to remove any epoll events.  If you don't call reset,
  // there could still be epoll events registered, or if you called
  // poison(), the file descriptor number could still be poisoned when
  // it is recycled by another socket.
  void reset(BorrowedFd fd) { setmode(getfd(fd), FdState::Mode::kFree); }

  // Make any pending and future callbacks return with EBADF untill
  // reset() is called.  Pending callbacks are made asynchronously, so
  // may still be running after poison() returns.
  void poison(BorrowedFd fd) { setmode(getfd(fd), FdState::Mode::kPoisoned); }

  // Read len bytes from fd and append them to *out.  If full is true,
  // will only read less than n bytes if there is an EOF.  Otherwise,
  // if full is false, it will return however many bytes it got from
  // the first successful call to read.
  Async<std::expected<size_t, Errno>> read(BorrowedFd fd, Buf *out, size_t len,
                                           bool full = true);

  // Write all of data to file descriptor fd.  The coroutine keeps
  // suspending itself until the entirety of data has been written or
  // an error occurs.  Always returns either data.size() or an Errno
  // other than EAGAIN.
  Async<std::expected<size_t, Errno>> write(BorrowedFd fd, BufView data);

  // Accept a new connection on a listening file descriptor lfd,
  // suspending the coroutine if no incoming connection is available.
  template<typename Addr = sockaddr>
  Async<std::expected<int, Errno>>
  accept(BorrowedFd lfd, Addr *addr = nullptr, socklen_t *addrlen = nullptr,
         int flags = SOCK_NONBLOCK | SOCK_CLOEXEC)
  {
    while (!st_.stop_requested()) {
      if constexpr (!std::same_as<decltype(addr), std::nullptr_t>)
        if (addrlen)
          *addrlen = sizeof(*addr);
      read_clear(lfd);
      int fd = accept4(lfd.get(), reinterpret_cast<sockaddr *>(addr), addrlen,
                       flags);
      if (fd >= 0)
        co_return fd;
      if (Errno err{}; err != EAGAIN)
        co_return err;
      if (auto err = co_await read_wait(lfd))
        co_return err;
    }
    co_return Errno{EBADF};
  }

  Async<std::expected<int, Errno>> tcpconnect(sockaddr *sa, socklen_t len);

  // Connect and return a file descriptor or a system error.
  Async<std::expected<int, std::system_error>> tcpconnect(std::string host,
                                                          std::string port);

  friend class RefPtr<CoroPoll>;

protected:
  void loop();
  void check(int timeout = -1);
  static BorrowedFd always_eof();
  void wake();

private:
  static inline constexpr std::uint32_t epflags[2] = {EPOLLIN, EPOLLOUT};

  struct FdState {
    enum Mode { kFree, kActive, kPoisoned };
    std::atomic<Mode> mode{kFree};
    std::atomic<std::uint32_t> ready{0};
    // callbacks: [0] = reader, [1] = writer
    std::array<std::atomic<WorkItem>, 2> callbacks{};

    FdState() noexcept = default;
    FdState(const FdState &other) { *this = other; }
    FdState &operator=(const FdState &other)
    {
      mode = other.mode.load();
      ready = other.ready.load();
      for (size_t i = 0; i < callbacks.size(); ++i)
        callbacks[i] = other.callbacks[i].load();
      return *this;
    }
  };

  UniqueFd epollfd_;

  std::shared_mutex resize_lock_;
  std::atomic<FdState *> state_{nullptr};
  std::atomic_size_t state_size_{0};
  std::atomic_uint want_resize_{0};
  std::atomic_uint numcoro_{0};

  std::stop_token st_;
  std::jthread pollthread_;

  struct FdPtr {
    FdState *p_;
    std::shared_lock<std::shared_mutex> lk_;
    FdState *get() const { return p_; }
    FdState *operator->() const { return p_; }
  };
  FdPtr getfd(BorrowedFd fd);

  void clear(BorrowedFd fd, bool write);
  void setcb(BorrowedFd fd, bool write, WorkItem h);
  void setmode(const FdPtr &fd, FdState::Mode mode);
  void docb(const FdPtr &fdp, bool write)
  {
    using enum FdState::Mode;
    FdState::Mode mode = fdp->mode;
    if ((fdp->ready & epflags[write]) || mode != kActive)
      if (WorkItem wi = fdp->callbacks[write].exchange(nullptr)) {
	--numcoro_;
	wi(mode == kActive ? 0 : EBADF);
      }
  }
  void docbs(const FdPtr &fdp)
  {
    docb(fdp, false);
    docb(fdp, true);
  }
};

void set_nonblock(BorrowedFd fd, bool on = true);

UniqueFd tcplisten(std::uint16_t port);

} // namespace x23
