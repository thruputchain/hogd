// -*- C++ -*-

#pragma once

#include <map>
#include <ranges>

#include <x23/fastclock.h>
#include <x23/threadpool.h>

namespace x23 {

// A queue of sleeping coroutines.  Coroutines can put themselves to
// sleep for a duration by co_awaiting the sleep method, or sleep
// until a specific time point by co_awaiting sleep_until() method.
// Immediately wakes all sleeping coroutines if it is destroyed.
// co_awaiting sleep and sleep_until returns a bool that is false if
// the CoroSleep was destroyed and the requested time might not have
// elapsed.
class CoroSleep {
public:
  using clock = fastclock;
  using Job = ThreadPool::Job;

  CoroSleep(ThreadPool &pool)
    : pool_(pool), service_thread_(&CoroSleep::service_loop, this)
  {}

  ~CoroSleep()
  {
    lock_.lock();
    done_.store(true, std::memory_order_relaxed);
    recheck_.notify_all();
    lock_.unlock();

    service_thread_.join();
  }

  struct [[nodiscard]] Awaiter {
    CoroSleep *cs_;
    clock::time_point wake_time_;

    // Always attempt to suspend
    static bool await_ready() noexcept { return false; }

    // Insert coroutine in waiters.  Returns false (meaning don't
    // suspend) when CoroSleep is being destroyed.
    bool await_suspend(CoroHandle h) const noexcept
    {
      return cs_->insert(wake_time_, h);
    }

    // Return true while the CoroSleep is active, or false if it is
    // being destroyed and the wake time may not have passed.
    bool await_resume() const noexcept
    {
      return !cs_->done_.load(std::memory_order_relaxed);
    }
  };
  Awaiter sleep_until(clock::time_point wake_time)
  {
    return {.cs_ = this, .wake_time_ = wake_time};
  }
  Awaiter sleep(clock::duration d) { return sleep_until(clock::now() + d); }

  ThreadPool &pool_;

private:
  // Return the next wakeup time.  Assumes lock_ is already held.
  clock::time_point next_wake() const
  {
    auto it = waiters_.begin();
    if (it == waiters_.end())
      return clock::time_point::max();
    return it->first;
  }

  // Return a multimap of all coroutines whose wake time has arrived.
  // If the CoroSleep is being destroyed, returns all coroutines.
  // Assumes lock_ is already held.
  std::multimap<clock::time_point, Job> ready_coros()
  {
    if (done_.load(std::memory_order_relaxed))
      return std::exchange(waiters_, {});

    decltype(waiters_) ready;
    auto it = waiters_.begin();
    auto end = waiters_.upper_bound(clock::now());
    while (it != end)
      ready.insert(waiters_.extract(it++));
    return ready;
  }

  // Insert a new waiter.  Fails and returns false if CoroSleep is
  // being destroyed.
  bool insert(clock::time_point wake_time, Job h)
  {
    std::lock_guard _lg(lock_);
    if (done_.load(std::memory_order_relaxed))
      return false;

    bool need_wake = wake_time < next_wake();
    waiters_.emplace(wake_time, h);
    if (need_wake)
      recheck_.notify_all();
    return true;
  }

  // Service routine for dedicated wakeup thread
  void service_loop()
  {
    std::unique_lock lk(lock_);
    for (;;)
      if (auto ready = ready_coros(); !ready.empty()) {
        // Release lock so resumed coros can call sleep() without deadlock
        lk.unlock();
        pool_.go(ready | std::views::values |
                 std::ranges::to<std::vector<Job>>());
        lk.lock();
      }
      else if (!done_.load(std::memory_order_relaxed))
        recheck_.wait_until(lk, next_wake());
      else
        return;
  }

  // Lock to protect waiters_ and done_.
  std::mutex lock_;

  // True when CoroSleep is being destroyed.  Atomic because
  // Awaiter::await_resume could load done_ concurrently with a store
  // in ~CoroSleep.  Even though we don't care what value load returns
  // in that situation, the spec says any such data race has undefined
  // behavior.
  std::atomic_bool done_;

  // All sleeping coroutines, indexed by wake time
  std::multimap<clock::time_point, Job> waiters_;

  // Notified when inserting a coroutine with an earlier wake_time.
  std::condition_variable recheck_;

  // Dedicated thread to resume coroutines as they become ready
  std::thread service_thread_;
};

} // namespace x23
