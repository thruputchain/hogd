// -*- C++ -*-

#pragma once

#include <map>
#include <vector>

#include <x23/config.h>
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
  using Clock = fastclock;

  CoroSleep(ThreadPool &pool)
    : pool_(pool), service_thread_(&CoroSleep::service_loop, this)
  {}

  ~CoroSleep()
  {
    lock_.lock();
    done_ = true;
    recheck_.notify_all();
    lock_.unlock();

    service_thread_.join();
  }

  struct [[nodiscard]] Awaiter {
    CoroSleep *cs_;
    Clock::time_point wake_time_;
    bool active_ = true;

    // Always attempt to suspend
    static bool await_ready() noexcept { return false; }

    // Insert coroutine in waiters.  Returns false (meaning don't
    // suspend) when CoroSleep is being destroyed.
    bool await_suspend(CoroHandle h) noexcept
    {
      return cs_->insert(wake_time_, h, &active_);
    }

    // Return true while the CoroSleep is active, or false if it is
    // being destroyed and the wake time may not have passed.
    bool await_resume() const noexcept { return active_; }
  };
  Awaiter sleep_until(Clock::time_point wake_time)
  {
    return {.cs_ = this, .wake_time_ = wake_time};
  }
  Awaiter sleep(Clock::duration d) { return sleep_until(Clock::now() + d); }

  ThreadPool &pool_;

private:
  using Job = ThreadPool::Job;

  struct Waiter {
    Job job_;
    bool *active_;
  };

  // Return the next wakeup time.  Assumes lock_ is already held.
  Clock::time_point next_wake() const
  {
    auto it = waiters_.begin();
    if (it == waiters_.end())
      return Clock::time_point::max();
    return it->first;
  }

  // Return a vector of all coroutines whose wake time has arrived.
  // If the CoroSleep is being destroyed, returns all coroutines.
  // Assumes lock_ is already held.
  std::vector<Job> ready_coros()
  {
    std::vector<Job> ready;
    if (done_) {
      ready.reserve(waiters_.size());
      for (auto &[time, waiter] : waiters_) {
        *waiter.active_ = false;
        ready.emplace_back(waiter.job_);
      }
      waiters_.clear();
      return ready;
    }

    auto it = waiters_.begin();
    auto end = waiters_.upper_bound(Clock::now());
    while (it != end) {
      ready.emplace_back(it->second.job_);
      it = waiters_.erase(it);
    }
    return ready;
  }

  // Insert a new waiter.  Fails and returns false if CoroSleep is
  // being destroyed.
  bool insert(Clock::time_point wake_time, Job h, bool *active)
  {
    std::lock_guard _lg(lock_);
    if (done_) {
      *active = false;
      return false;
    }

    bool need_wake = wake_time < next_wake();
    waiters_.emplace(wake_time, Waiter{.job_ = h, .active_ = active});
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
        pool_.go(std::move(ready));
        lk.lock();
      }
      else if (!done_)
        recheck_.wait_until(lk, next_wake());
      else
        return;
  }

  // Lock to protect waiters_ and done_.
  std::mutex lock_;

  // True when CoroSleep is being destroyed.
  bool done_ = false;

  // All sleeping coroutines, indexed by wake time
  std::multimap<Clock::time_point, Waiter> waiters_;

  // Notified when inserting a coroutine with an earlier wake_time.
  std::condition_variable recheck_;

  // Dedicated thread to resume coroutines as they become ready
  std::thread service_thread_;
};

} // namespace x23
