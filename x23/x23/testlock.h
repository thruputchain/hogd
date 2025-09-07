// -*-C++-*-

#pragma once

#include <atomic>
#include <cassert>
#include <mutex>
#include <utility>

#include <x23/refcnt.h>
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

// A shared version of TestLock.  The constructor argument describes
// how to group lockers, so that different shared lockers can touch
// different cache lines.
//
// *Not* compatible with std::shared_lock, because it needs to
// remember which bucket was referenced by a shared locker.  To keep
// this per-locker state around, it returns a shared_lock-like RAII
// type.
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
    TestLock *tl_; // Could get away without, but we have a whole cache line

    Inhibitor(TestLock *tl) noexcept : tl_(tl) {}

    void incref() noexcept
    {
      for (;;) {
        count.fetch_add(1);
        if (!tl_->busy())
          return;
        decref();
        tl_->block_until_free();
      }
    }
    void decref() noexcept
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
  explicit SharedLock(const CpuMap &map = *llc_map) : inhibitors_(map)
  {
    inhibitors_.populate(this);
  }

  const CpuMap &map() const noexcept { return inhibitors_.map_; }

  void block_while_inhibited() noexcept
  {
    for (auto &inh : inhibitors_)
      inh.block();
  }

  // With coroutines, there's a risk of the lock being released in a
  // different thread from the one in which it was acquired.  So
  // instead of providing an explicit unlock method, return a Holder
  // that remembers in which thread the lock was acquired and releases
  // it on destruction.
  class [[nodiscard]] SharedHolder : private RefPtr<Inhibitor> {
    using super = RefPtr<Inhibitor>;

  public:
    SharedHolder() noexcept : super(nullptr) {}
    SharedHolder(SharedLock &sl) noexcept : super(sl.inhibitors_.lookup()) {}
    SharedHolder(SharedHolder &&) noexcept = default;
    using super::operator=;
    using super::reset;
    friend detail::RefOps<super>;
  };

  // Don't call it lock_shared() because the interface is different
  // from shared_mutex.
  SharedHolder hold_shared() noexcept { return *this; }

protected:
  PerCpuSet<Inhibitor> inhibitors_;
};

} // namespace x23
