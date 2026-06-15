// -*-C++-*-

#pragma once

#include <atomic>
#include <mutex>

#include <x23/cpumap.h>
#include <x23/debug.h>
#include <x23/refcnt.h>
#include <x23/threadpool.h>
#include <x23/utility.h>

#define X23_TRACE_SHLOCK 0

namespace x23 {

// A simple futex-based mutex supporting some features that are
// missing from std::mutex:
//
// * As the name suggests, you can *test* whether or not the lock is
//   busy without acquiring or exclusively caching it.  The busy()
//   method tells you whether a TestLock is locked.
//
// * You can unlock the TestLock in a different thread from the one
//   in which you acquired it, which is important for coroutines that
//   may migrate across threads.  Note you still can't lock a
//   TestLock in a coroutine executing in ThreadPool, as coroutines
//   should never block, but it is safe to use try_lock().
//
// * There's a block_while_inhibited() hook for derived types
//   implementing shared locks.
//
// * Because TestLock is really small (1 byte, compared to 40 bytes
//   for std::mutex), you can more easily fit it on the same cache
//   line as what you are locking.
//
// TestLock has the same interface as std::mutex, so it can be used
// with std::lock_guard or std::unique_lock.
//
// Unlike std::mutex, there could be undefined behavior if you destroy
// a TestLock right after unlocking it when another thread is still
// unlocking it.  In this case, the second-to-last unlocker could call
// atomic::notify_all() on a destroyed atomic.  Calling methods on
// destroyed objects is UB, even though the underlying FUTEX_WAKE
// system call is fine, so this is probably fine in practice.
class TestLock {
public:
  using Holder = std::unique_lock<TestLock>;

  bool try_lock(this auto &&self, bool want_notify = false) noexcept
  {
    auto prev = self.status_.exchange(kLocked, std::memory_order_acquire);
    x23assert(prev == kUnlocked || prev == kLocked ||
              prev == kLockedWantNotify);
    if (prev == kLockedWantNotify || (prev == kLocked && want_notify))
      prev =
          self.status_.exchange(kLockedWantNotify, std::memory_order_acquire);
    if (prev != kUnlocked)
      return false;
#if X23_TRACE_SHLOCK
    self.owner_ = std::this_thread::get_id();
#endif // X23_TRACE_SHLOCK
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
#if X23_TRACE_SHLOCK
    owner_ = {};
#endif // X23_TRACE_SHLOCK
    if (auto old = status_.exchange(kUnlocked); old == kLockedWantNotify)
      status_.notify_all();
    else
      x23assert(old == kLocked);
  }

  bool busy() const noexcept
  {
    return status_.load(std::memory_order_acquire) != kUnlocked;
  }
  void block_until_free() noexcept
  {
    for (;;) {
      auto prev = status_.load(std::memory_order_acquire);
      if (prev == kLocked)
        status_.compare_exchange_weak(prev, kLockedWantNotify,
                                      std::memory_order_acquire);
      else
        x23assert(prev == kUnlocked || prev == kLockedWantNotify);
      if (prev == kUnlocked)
        return;
      status_.wait(kLockedWantNotify);
    }
  }

private:
#ifdef NDEBUG
  static constexpr uint8_t kUnlocked = 0;
  static constexpr uint8_t kLocked = 1;
  static constexpr uint8_t kLockedWantNotify = 2;
#else  // !NDEBUG
  static constexpr uint8_t kUnlocked = 0x3b;
  static constexpr uint8_t kLocked = 0x9e;
  static constexpr uint8_t kLockedWantNotify = 0xf5;
#endif // !NDEBUG
  std::atomic_uint8_t status_{kUnlocked};
#if X23_TRACE_SHLOCK
  std::thread::id owner_{};
#endif // X23_TRACE_SHLOCK
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
    std::atomic_uint want_wake{0};
    TestLock *tl_; // Could get away without, but we have a whole cache line
#if X23_TRACE_SHLOCK
    std::array<std::atomic<std::thread::id>, 2> slockers_;
#endif // X23_TRACE_SHLOCK

    Inhibitor(TestLock *tl) noexcept : tl_(tl) {}

    void incref() noexcept
    {
      for (;;) {
        count.fetch_add(1);
        if (!tl_->busy()) {
#if X23_TRACE_SHLOCK
          auto id = std::this_thread::get_id();
          for (unsigned i = 0; i < slockers_.size(); ++i) {
            if (slockers_[i] == std::thread::id{} || slockers_[i] == id) {
              slockers_[i] = id;
              return;
            }
          }
          for (unsigned i = 0; i < slockers_.size() - 1; ++i)
            slockers_[i] = slockers_[i + 1].load();
          slockers_.back() = id;
#endif // X23_TRACE_SHLOCK
          return;
        }
        decref();
        tl_->block_until_free();
      }
    }
    void decref() noexcept
    {
      if (count.fetch_sub(1) == 1 && want_wake.load())
        count.notify_all();
#if X23_TRACE_SHLOCK
      auto id = std::this_thread::get_id();
      for (unsigned i = 0; i < slockers_.size(); ++i)
        if (slockers_[i] == id) {
          slockers_[i] = std::thread::id{};
          break;
        }
#endif // X23_TRACE_SHLOCK
    }
    void block() noexcept
    {
      if (auto n = count.load()) {
        ++want_wake;
        while ((n = count.load()))
          count.wait(n);
        --want_wake;
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
