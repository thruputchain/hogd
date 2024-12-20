// -*-C++-*-

#include <atomic>
#include <cassert>

#include <concepts>
#include <x23/threadpool.h>

namespace x23 {

// A simple futex-based mutex that, unlike std::mutex, lets you test
// whether or not the lock is busy without acquiring the lock or
// exclusively caching it.  Also provides a block_while_inhibited()
// hook for derived types implementing shared locks.
class TestLock {
public:
  using Holder = std::unique_lock<TestLock>;

  bool try_lock(this auto &&self, bool want_notify = false)
  {
    int prev = self.status_.exchange(kLocked, std::memory_order_acquire);
    if (prev == kLockedWantNotify || (prev == kLocked && want_notify))
      prev =
          self.status_.exchange(kLockedWantNotify, std::memory_order_acquire);
    if (prev != kUnlocked)
      return false;
    if constexpr (requires { self.block_while_inhibited(); })
      self.block_while_inhibited();
    return true;
  }
  void lock(this auto &&self)
  {
    while (!self.try_lock(true))
      self.status_.wait(kLockedWantNotify);
  }
  void unlock()
  {
    if (status_.exchange(kUnlocked) == kLockedWantNotify)
      status_.notify_all();
  }

  bool busy() const
  {
    return status_.load(std::memory_order_acquire) != kUnlocked;
  }
  void block_until_free()
  {
    for (;;) {
      int prev = status_.load(std::memory_order_acquire);
      if (prev == kLocked)
        status_.compare_exchange_weak(prev, kLockedWantNotify,
                                      std::memory_order_relaxed,
                                      std::memory_order_acquire);
      if (prev == kUnlocked)
        return;
      status_.wait(kLockedWantNotify);
    }
  }

private:
  static constexpr int kUnlocked = 0;
  static constexpr int kLocked = 1;
  static constexpr int kLockedWantNotify = 2;
  std::atomic_int status_{kUnlocked};
};

// A shared version of TestLock.  The constructor argument is the
// number of buckets for shared lockers, so that different shared
// lockers can touch different cache lines.
class SharedLock : public TestLock {
  // One or more if these Inhibitor counters are used to delay an
  // exclusive locker.
  struct alignas(hardware_destructive_interference_size) Inhibitor {
    std::atomic_uint count{0};
    std::atomic_bool want_wake{false};

    void inc(TestLock *tl)
    {
      for (;;) {
        count.fetch_add(1);
        if (!tl->busy())
          return;
        dec();
        tl->block_until_free();
      }
    }
    void dec()
    {
      if (count.fetch_sub(1) == 0 && want_wake.load()) {
        want_wake = false;
        count.notify_all();
      }
    }
    void block()
    {
      while (auto n = count.load()) {
        want_wake = true;
        count.wait(n);
      }
    }
  };

public:
  const unsigned nshared_;

  explicit SharedLock(unsigned nshared = 1)
    : nshared_(nshared), inhibitors_(new Inhibitor[nshared_])
  {
    assert(nshared);
  }
  ~SharedLock() { delete[] inhibitors_; }

  void block_while_inhibited()
  {
    for (size_t i = 0; i < nshared_; ++i)
      inhibitors_[i].block();
  }

  // With coroutines, there's a risk of the lock being released in a
  // different thread from the one in which it was acquired.  So
  // instead of providing an explicit unlock method, return a Holder
  // that remembers in which thread the lock was acquired and releases
  // it on destruction.
  class [[nodiscard]] SharedHolder {
  public:
    SharedHolder() noexcept = default;
    SharedHolder(SharedLock &sl) noexcept
      : inh_(&sl.inhibitors_[ThreadPool::bucket_index(sl.nshared_)])
    {
      inh_->inc(&sl);
    }
    SharedHolder(SharedHolder &&other) noexcept
      : inh_(std::exchange(other.inh_, nullptr))
    {}
    ~SharedHolder() { reset(); }

    SharedHolder &operator=(SharedHolder &&other) noexcept
    {
      reset();
      inh_ = std::exchange(other.inh_, nullptr);
      return *this;
    }

    // Release the lock immediately (if Holder contains one)
    void reset()
    {
      if (inh_) {
        inh_->dec();
        inh_ = nullptr;
      }
    }

  private:
    struct Inhibitor *inh_{};
  };

  // Don't call it lock_shared() because the interface is different
  // from shared_mutex.
  SharedHolder hold_shared() { return *this; }

protected:
  Inhibitor *inhibitors_;
};

// A CoroSharedLock is a TestLock (or SharedLock) that also provides
// coro_lock() and coro_shared_lock() methods allowing coroutines to
// acquire the lock without blocking.  Both coro_lock and
// coro_shared_lock return "holder" values you must keep in scope
// until you are done with the lock.  The shared lock is only good
// until the next time your coroutine is suspended, but as a result is
// heavily optimized within coroutines.
//
// Note this is optimized for shared locks, not exclusive locks.
template<typename Super = TestLock> class CoroSharedLock : public Super {
public:
  ThreadPool &pool_;

  template<typename... Args> requires std::constructible_from<Super, Args...>
  explicit CoroSharedLock(ThreadPool &pool, Args &&...args)
    : Super(std::forward<Args>(args)...), pool_(pool)
  {}

  void block_while_inhibited()
  {
    pool_.drain();
    if constexpr (requires { this->Super::block_while_inhibited(); })
      this->Super::block_while_inhibited();
  }

  using CoroHolder = typename Super::Holder;
  Async<CoroHolder> coro_lock()
  {
    return pool_.async([this] { return CoroHolder(*this); });
  }

  // Note: there's nothing to do to unlock shared, because you give up
  // the lock as soon as the coroutine is suspended.  But it's a
  // serious error to hold the lock across suspension points, so we
  // return a CrashIfSuspended.
  using CoroSharedHolder = ThreadPool::CrashIfSuspended;
  Async<CoroSharedHolder> coro_lock_shared()
  {
    if (ThreadPool::get_thread_pool() != &pool_)
      co_await pool_.bg();
    Super::block_until_free();
    co_return ThreadPool::CrashIfSuspended{};
  }
};

// A pointer that is good only until you suspend the current
// coroutine, which is the duration of the mutual exclusion provided
// by CoroSharedLock::coro_lock_shared.
template<typename T> class CoroTempPtr {
public:
  using value_type = T;

  CoroTempPtr(T *p = nullptr) : p_(p) { init(); }
  CoroTempPtr &operator=(T *p)
  {
    p_ = p;
    init();
    return *this;
  }

  T *get() const
  {
    check();
    return p_;
  }
  operator T &() const { return *get(); }
  T *operator->() const { return get(); }

private:
  T *p_;
#ifndef NDEBUG
  ThreadPool::SuspendCount sc_;
  void init() { sc_ = ThreadPool::get_suspend_count(); }
  void check() const { assert(!p_ || sc_ == ThreadPool::get_suspend_count()); }
#else  // NDEBUG
  void init() {}
  void check() const {}
#endif // NDEBUG
};

} // namespace x23
