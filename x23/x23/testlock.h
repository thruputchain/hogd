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
      while (auto n = inhibitors_[i].count.load()) {
        inhibitors_[i].want_wake = true;
        inhibitors_[i].count.wait(n);
      }
  }

  void lock_shared()
  {
    Inhibitor &inh = inhibitors_[my_bucket()];
    for (;;) {
      inh.count.fetch_add(1, std::memory_order_acquire);
      if (!busy())
        return;
      unlock_shared();
      block_until_free();
    }
  }
  void unlock_shared()
  {
    Inhibitor &inh = inhibitors_[my_bucket()];
    if (inh.count.fetch_sub(1) == 0 && inh.want_wake.load()) {
      inh.want_wake = false;
      inh.count.notify_all();
    }
  }

protected:
  struct alignas(hardware_destructive_interference_size) Inhibitor {
    std::atomic_uint count{0};
    std::atomic_bool want_wake{false};
  };
  Inhibitor *inhibitors_;

  size_t my_bucket() const
  {
    constexpr std::hash<std::thread::id> hasher;
    return hasher(std::this_thread::get_id()) % nshared_;
  }
};

// A CoroSharedLock is a SharedLock that also provides coro_lock() and
// coro_shared_lock() methods allowing coroutines to acquire the lock
// without blocking.  Both coro_lock and coro_shared_lock return
// values you must keep in scope until you are done with the lock.
// The shared lock is only good until the next time your coroutine is
// suspended, but as a result is heavily optimized within coroutines.
//
// Note this is heavily optimized for shared locks rather than
// exclusie locks.
template<typename Super = SharedLock> class CoroSharedLock : public Super {
public:
  ThreadPool &pool_;

  template<typename... Args> requires std::constructible_from<Super, Args...>
  explicit CoroSharedLock(ThreadPool &pool, Args &&...args)
    : Super(std::forward<Args>(args)...), pool_(pool)
  {}

  void block_while_inhibited()
  {
    pool_.drain();
    if constexpr (requires { Super::block_while_inhibited(); })
      Super::block_while_inhibited();
  }

  Async<std::unique_lock<Super>> coro_lock()
  {
    co_await pool_.async([this] { this->lock(); });
    co_return std::unique_lock<Super>{*this, std::adopt_lock};
  }

  Async<ThreadPool::CrashIfSuspended> coro_lock_shared()
  {
    if (ThreadPool::get_thread_pool() != &pool_)
      co_await pool_.bg();
    Super::block_until_free();
    co_return ThreadPool::CrashIfSuspended{};
    // Note: there's nothing to do to unlock shared, because you give
    // up the lock as soon as the coroutine is suspended.
  }
};

} // namespace x23
