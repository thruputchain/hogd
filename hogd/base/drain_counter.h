// drain_counter.h - count in-flight operations and await quiescence

#pragma once

#include <atomic>
#include <cstdint>

#include "hogd/base/log.h"
#include "x23/corotrace.h"
#include "x23/debug.h"

namespace hogd {

// DrainCounter counts operations that are in flight and lets one coroutine at a
// time wait, without spinning or holding a thread, for the count to reach zero.
//
// The waiter is resumed inline by the done() call that retires the last
// operation, on that caller's thread.  This is the convention x23 already uses
// to resume an RPC client from its connection's reader, and it keeps the waiter
// from queueing behind whatever else the thread pool is running.  The flip side
// is that done() runs the waiter's continuation before it returns: call it with
// no locks held, and touch nothing the waiter might free (including the
// DrainCounter itself) after it returns.
//
// Operations may be added while a waiter is parked and after a wait has
// completed.  A wait completes when the count is observed at zero, so it covers
// every operation added before that point.  The counter can be waited on any
// number of times, by one waiter at a time.
//
// The count and the "a waiter is parked" flag share one atomic word, so the
// done() that takes the count to zero also claims the waiter in the same
// atomic step.  A done() that does not claim it touches nothing after its
// update, and the waiter cannot run until the claiming done() resumes it.
// That is what makes it safe for the waiter to destroy the counter's owner as
// soon as it resumes, while done() calls may still be returning.
class DrainCounter {
public:
  DrainCounter() = default;
  DrainCounter(DrainCounter const &) = delete;
  DrainCounter &operator=(DrainCounter const &) = delete;

  ~DrainCounter()
  {
    x23assert((state_.load(std::memory_order_relaxed) & kWaiting) == 0);
  }

  // Registers one in-flight operation.
  void add() noexcept
  {
    [[maybe_unused]] uint64_t prev =
        state_.fetch_add(1, std::memory_order_release);
    x23assert((prev & kCountMask) != kCountMask);
  }

  // Retires one in-flight operation.  If it was the last one and a waiter is
  // parked, resumes the waiter before returning (see the class comment).
  void done() noexcept
  {
    uint64_t cur = state_.load(std::memory_order_relaxed);
    uint64_t next = 0;
    do {
      CHECK((cur & kCountMask) != 0);
      next = cur == (kWaiting | 1) ? 0 : cur - 1;
    } while (!state_.compare_exchange_weak(cur, next, std::memory_order_acq_rel,
                                           std::memory_order_relaxed));
    if (cur == (kWaiting | 1)) {
      // We cleared kWaiting, so the parked waiter is ours alone to resume, and
      // it stays suspended (keeping waiter_ alive) until we do.
      x23::CoroHandle h = waiter_;
      h.resume();
      // *this may already be gone.
    }
  }

  // Operations currently in flight.  Only a snapshot unless the caller knows
  // nothing can add or retire one concurrently.
  uint32_t in_flight() const noexcept
  {
    return static_cast<uint32_t>(state_.load(std::memory_order_acquire) &
                                 kCountMask);
  }

  class [[nodiscard]] IdleAwaiter {
  public:
    explicit IdleAwaiter(DrainCounter &c) noexcept : c_(c) {}

    bool await_ready() const noexcept
    {
      return (c_.state_.load(std::memory_order_acquire) & kCountMask) == 0;
    }

    bool await_suspend(x23::CoroHandle h SLOC_DECL) noexcept
    {
#if X23_CORO_TRACE
      if (x23::PromiseBase *base = h.base())
        base->suspension_ = sloc;
#endif // X23_CORO_TRACE
      c_.waiter_ = h;
      uint64_t cur = c_.state_.load(std::memory_order_acquire);
      do {
        if ((cur & kCountMask) == 0)
          return false; // drained since await_ready(): don't suspend
        x23assert((cur & kWaiting) == 0); // one waiter at a time
      } while (!c_.state_.compare_exchange_weak(cur, cur | kWaiting,
                                                std::memory_order_release,
                                                std::memory_order_acquire));
      // Parked.  The done() that retires the last operation may already be
      // resuming us on another thread, so neither this awaiter nor c_ may be
      // touched from here on.
      return true;
    }

    void await_resume() const noexcept {}

  private:
    DrainCounter &c_;
  };

  // co_await wait_idle() completes once no operation is in flight.  The memory
  // effects of every operation retired before then happen-before the waiter
  // continues.
  IdleAwaiter wait_idle() noexcept { return IdleAwaiter{*this}; }

private:
  static constexpr uint64_t kCountMask = 0xffff'ffff;
  static constexpr uint64_t kWaiting = uint64_t{1} << 63;

  std::atomic<uint64_t> state_{0};
  // Written by the waiter before it publishes kWaiting (release), and read only
  // by the done() whose acquire CAS clears it.
  x23::CoroHandle waiter_;
};

} // namespace hogd
