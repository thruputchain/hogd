// -*-C++-*-

#include <atomic>
#include <cassert>

#include <x23/threadpool.h>

namespace x23 {

// A simple futex-based mutex that, unlike std::mutex, lets you test
// whether or not the lock is busy without acquiring the lock or
// exclusively caching it.  Also provides a wait_inhibit() hook for
// derived types implementing shared locks.  Not optimized for the
// case where you need to acquire the lock with any frequency.
class TestLock {
public:
  bool try_lock(this auto &&self)
  {
    if (self.flag_.test_and_set(std::memory_order_acquire))
      return false;
    if constexpr (requires { self.wait_inhibit(); })
      self.wait_inhibit();
    return true;
  }
  void lock(this auto &&self)
  {
    while (!self.try_lock())
      self.flag_.wait(true);
  }
  void unlock()
  {
    flag_.clear(std::memory_order_release);
    flag_.notify_all(); // not efficient if lock/unlock common
  }

  bool busy() const { return flag_.test(std::memory_order_acquire); }
  void block_until_free()
  {
    while (busy())
      flag_.wait(true);
  }

private:
  std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

class SharedLock : public TestLock {
public:
  const unsigned nshared_;

  explicit SharedLock(unsigned nshared = 1)
    : nshared_(nshared), inhibitors_(new Inhibitor[nshared_])
  {
    assert(nshared);
  }
  ~SharedLock() { delete[] inhibitors_; }

  void wait_inhibit()
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
      inh.count.fetch_add(1);
      if (!busy())
	return;
      unlock_shared();
      block_until_free();
    }
  }
  void unlock_shared()
  {
    Inhibitor &inh = inhibitors_[my_bucket()];
    if (inh.count.fetch_sub(1) == 0
	&& inh.want_wake.load()) {
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

  size_t my_bucket() const {
    constexpr std::hash<std::thread::id> hasher;
    return hasher(std::this_thread::get_id()) % nshared_;
  }
};

} // namespace x23
