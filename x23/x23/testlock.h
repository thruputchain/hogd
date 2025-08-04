// -*-C++-*-

#pragma once

#include <atomic>
#include <cassert>
#include <mutex>
#include <utility>

#include <x23/threadpool.h>
#include <x23/utility.h>

namespace x23 {

// A simple futex-based mutex that, unlike std::mutex, lets you test
// whether or not the lock is busy without acquiring the lock or
// exclusively caching it.  Also provides a block_while_inhibited()
// hook for derived types implementing shared locks.
//
// Has the same interface as std::mutex, so can be used with
// std::lock_guard.
class TestLock {
public:
  using Holder = std::unique_lock<TestLock>;

  bool try_lock(this auto &&self, bool want_notify = false) noexcept
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
  void lock(this auto &&self) noexcept
  {
    while (!self.try_lock(true))
      self.status_.wait(kLockedWantNotify);
  }
  void unlock() noexcept
  {
    if (status_.exchange(kUnlocked) == kLockedWantNotify)
      status_.notify_all();
  }

  bool busy() const noexcept
  {
    return status_.load(std::memory_order_acquire) != kUnlocked;
  }
  void block_until_free() noexcept
  {
    for (;;) {
      int prev = status_.load(std::memory_order_acquire);
      if (prev == kLocked)
        status_.compare_exchange_weak(prev, kLockedWantNotify,
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
//
// *Not* compatible with std::shared_lock because it needs to keep
// some per-locker state around, so instead it returns a
// shared_lock-like RAII type.
//
//   SharedLock sl;
//
//   f() {
//     {
//       auto holder = sl.hold_shared();
//       // lock held in shared mode
//     }
//   }
//
//   g() {
//     auto holder = sl.hold_shared();
//     // lock held in shared mode
//     holder.reset();
//     // lock released
//   }
class SharedLock : public TestLock {
  // One or more of these Inhibitor counters are used to delay an
  // exclusive locker.
  struct alignas(hardware_destructive_interference_size) Inhibitor {
    std::atomic_uint count{0};
    std::atomic_bool want_wake{false};

    void inc(TestLock *tl) noexcept
    {
      for (;;) {
        count.fetch_add(1);
        if (!tl->busy())
          return;
        dec();
        tl->block_until_free();
      }
    }
    void dec() noexcept
    {
      if (count.fetch_sub(1) == 1 && want_wake.load()) {
        want_wake = false;
        count.notify_all();
      }
    }
    void block() noexcept
    {
      while (auto n = count.load()) {
        want_wake = true;
        count.wait(n);
      }
    }
  };

public:
  const unsigned nshared_;

  explicit SharedLock(unsigned nshared = 1) noexcept
    : nshared_(nshared), inhibitors_(new Inhibitor[nshared_])
  {
    assert(nshared);
  }
  ~SharedLock() { delete[] inhibitors_; }

  void block_while_inhibited() noexcept
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
      if (&other != this) {
        reset();
        inh_ = std::exchange(other.inh_, nullptr);
      }
      return *this;
    }

    // Release the lock immediately (if Holder contains one)
    void reset() noexcept
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
  SharedHolder hold_shared() noexcept { return *this; }

protected:
  Inhibitor *inhibitors_;
};

} // namespace x23
