// -*-C++-*-

// A lock that lets only one coroutine at a time do something.
// Unfortunately, because the C++ coroutine interface doesn't make all
// the information you want available all at once, the interface is a
// bit clunky and requires two lines of code do acquire a lock, one to
// declare a CoroGuard, another to acquire the lock:
//
//   CoroLock lock;
//
//   Detached
//   some_coroutine()
//   {
//     CoroGuard g;
//     co_await g(lock);  // acquire lock
//     do_something;
//     g.reset();         // release lock (can also let g go out of scope)
//   }
//
// A CoroGuard object cannot be moved, as it is updated by other
// threads.
//
// The basic strategy is that the CoroLock contains a pointer to
// LockerState for last coroutine in line to acquire the lock, or NULL
// if the lock is free.  LockerState contains the coroutine_handle of
// the next coroutine to wake up.  When acquiring the lock, a
// coroutine swaps itself into the last position pointer, then sets
// the coroutine_handle in the previous last coroutine's LockerState
// to point to itself.  On release, a coroutine resumes the next
// coroutine.
//
// The structure is reminiscent of MCS spinlocks, though CoroLock
// suspends the coroutine, making it more comparable to a sleeping
// lock than a spinlock.

#pragma once

#include <atomic>
#include <cassert>

#include <x23/async.h>
#include <x23/mvar.h>
#include <x23/threadpool.h>

namespace x23 {

class CoroLock {
public:
  // Every coroutine that wants to acquire the lock needs a LockerState
  class LockerState {
  public:
    LockerState() noexcept = default;
    LockerState(LockerState &&) = delete;

  private:
    atomic_coroutine_handle next{};
    friend CoroLock;
  };

  // Must be co_awaited.  Acquire the lock or suspend.  You need a
  // LockerState that will persist for the whole time you have the
  // lock.
  [[nodiscard]] auto lock(LockerState *ls)
  {
    // LockerState can only be used for one lock at a time
    assert(ls->next.load(std::memory_order_relaxed) == nullptr);

    struct AwaitAcquire {
      CoroLock &lk_;
      LockerState *next_;
      LockerState *prev_;

      AwaitAcquire(CoroLock &lk, LockerState *ls) : lk_(lk), next_(ls) {}
      bool await_ready() { return !(prev_ = lk_.last_.exchange(next_)); }
      void await_suspend(std::coroutine_handle<> h) { prev_->next = h; }
      void await_resume() {}
    };
    return AwaitAcquire{*this, ls};
  }

  // Release the lock and invoke the next coroutine waiting for the
  // lock before returning.
  void unlock(LockerState *ls)
  {
    if (std::coroutine_handle<> h = next(ls))
      h();
  }

private:
  template<typename T> struct AwaitReleaseAndReturn;

public:
  // Release the lock and immediately destroy the invoking coroutine,
  // resume the next coroutine in line for the lock.  If present, the
  // second argument is passed to return_value in coroutines returning
  // a value.
  [[nodiscard]] AwaitReleaseAndReturn<void> unlock_and_return(LockerState *ls);
  template<typename T>
  [[nodiscard]] AwaitReleaseAndReturn<T> unlock_and_return(LockerState *ls,
                                                           T t);

  // True while lock is held
  bool busy() const { return last_.load(std::memory_order_acquire); }

private:
  // If last_ is NULL, the lock is free.  If last_ points to a
  // LockerState then it belongs to the last coroutine in line to
  // acquire the lock, in which case a new coroutine can enqueue
  // itself by putting its own atomic_coroutine_handle into the last
  // LockerState and setting last_ to its own LockerState.
  std::atomic<LockerState *> last_{nullptr};

  std::coroutine_handle<> next(LockerState *ls)
  {
    if (last_.compare_exchange_strong(ls, nullptr)) {
      assert(ls->next.load(std::memory_order_relaxed) == nullptr);
      return nullptr;
    }
    std::coroutine_handle<> h;
    while (!(h = ls->next))
      detail::pause_instruction();
    ls->next.store(nullptr, std::memory_order_relaxed);
    return h;
  }
};

namespace detail {

// If we are in a thread pool, resume the coroutine in another thread.
// If not, resume it synchronously in this thread.
void resume_in_another_thread(std::coroutine_handle<>);

} // namespace detail

template<> struct [[nodiscard]] CoroLock::AwaitReleaseAndReturn<void> {
  CoroLock &lk_;
  LockerState *next_;

  AwaitReleaseAndReturn(CoroLock &lk, LockerState *ls) : lk_(lk), next_(ls) {}
  bool await_ready() noexcept { return false; }

  // Returns NULL if there is no other coroutine to resume, otherwise
  // returns the coroutine.
  template<typename Promise>
  std::coroutine_handle<>
  finish_suspend(std::coroutine_handle<Promise> moriturus) noexcept
  {
    std::coroutine_handle<> next_locker = lk_.next(next_);
    std::coroutine_handle<> next_coro = nullptr;
    if constexpr (requires {
                    next_coro = detail::emulate_final_suspend(moriturus);
                  })
      next_coro = detail::emulate_final_suspend(moriturus);
    else
      detail::emulate_final_suspend(moriturus);
    if (!next_coro)
      return next_locker ? next_locker : std::noop_coroutine();
    // If we reach here, the coroutine calling unlock_and_return
    // yields to another co_routine with a custom final_suspend()
    // awaiter whose await_suspend() returned next_coro.
    if (!next_locker)
      return next_coro;
    // If we reach here, we need to resume two coroutines.  If we are
    // in a thread pool, try to resume next_coro in another thread.
    // Otherwise, resume it synchronously before resuming the next
    // locker.  If you might not be in a threadpool and prefer to
    // resume next_locker first, just don't use unlock_and_return.
    // The normal behavior is to run the next locker as soon as you
    // release the lock (either explicitly or implicitly when a
    // CoroGuard is destroyed).
    detail::resume_in_another_thread(next_coro);
    return next_locker;
  }

  template<typename Promise>
  auto await_suspend(std::coroutine_handle<Promise> moriturus) noexcept
  {
    Promise &p = moriturus.promise();
    p.return_void(); // Coroutine must return void
    return finish_suspend(moriturus);
  }
  void await_resume() noexcept { std::terminate(); }
};
template<typename T>
struct [[nodiscard]] CoroLock::AwaitReleaseAndReturn
  : CoroLock::AwaitReleaseAndReturn<void> {
  using super = CoroLock::AwaitReleaseAndReturn<void>;
  T t_;

  AwaitReleaseAndReturn(CoroLock &lk, LockerState *ls, T t)
    : super(lk, ls), t_(std::move(t))
  {}
  template<typename Promise>
  auto await_suspend(std::coroutine_handle<Promise> moriturus) noexcept
  {
    Promise &p = moriturus.promise();
    p.return_value(std::move(t_));
    return finish_suspend(moriturus);
  }
};

inline CoroLock::AwaitReleaseAndReturn<void>
CoroLock::unlock_and_return(CoroLock::LockerState *ls)
{
  return AwaitReleaseAndReturn<void>{*this, ls};
}
template<typename T>
inline CoroLock::AwaitReleaseAndReturn<T>
CoroLock::unlock_and_return(LockerState *ls, T t)
{
  return AwaitReleaseAndReturn<T>{*this, ls, std::move(t)};
}

// A CoroSharedLock is a shared lock optimized for shared locking.  It
// requires that shared lockers not suspend themselves.  Hence,
// exclusive lockers need only hold the lock and wait for all shared
// lockers to suspend themselves.  CoroSharedLock is intended to be
// used for structures like vectors that are very infrequently
// resized.  In the common case of no exclusive lockers, there is no
// need for any coherence misses (as might happen in an alternate
// design that keeps a count of shared lockers in an atomic int).
class CoroSharedLock : public CoroLock {
public:
  ThreadPool &pool_;

  explicit CoroSharedLock(ThreadPool &pool) : pool_(pool) {}
  Async<void> lock(LockerState *ls)
  {
    co_await CoroLock::lock(ls);
    co_await pool_.async(&ThreadPool::drain, &pool_);
    co_return;
  }

  // unlock is unmodified from the superclass

  Async<void> lock_shared()
  {
    for (;;) {
      if (ThreadPool::get_thread_pool() != &pool_)
	co_await pool_.bg();
      if (!busy())
	co_return;
      LockerState ls;
      co_await lock(&ls);
      unlock(&ls);	    // resumes next coroutine before returning
    }
  }

  // There's no unlock_shared, because you give up the lock as soon as
  // the coroutine is suspended.
};

// A CoroGuard is similar to a lock_guard for a CoroLock.  However,
// the syntax is different because a CoroLock requires LockerState to
// persist for the duration of the lock, so the CoroGuard holds that
// state.  To ensure the state lives until the lock is released, the
// CoroGuard must be declared before the lock is acquired, as in:
//
//   CoroLock lk;
//   ...
//
//     CoroGuard g;
//     co_await g(lk);
struct CoroGuard : CoroLock::LockerState {
  CoroLock *lkp_{};

  CoroGuard() noexcept = default;
  ~CoroGuard() { reset(); }

  // Must be co_awaited and must be called on lvalue CoroGuard
  [[nodiscard]] auto operator()(std::derived_from<CoroLock> auto &lk) &
  {
    assert(!lkp_);
    lkp_ = &lk;
    return lkp_->lock(this);
  }

  void reset()
  {
    if (lkp_)
      std::exchange(lkp_, nullptr)->unlock(this);
  }

  // reset_and_return can only be called when the CoroGuard contains a
  // valid lock.  The return value must be co_awaited and immediately
  // destroys the invoking coroutine.  Otherwise, if you are at the
  // end of a coroutine and release the lock normally before a
  // co_return statement, the invoking coroutine will not be destroyed
  // until the next locker suspends itself.  The version with an
  // argument returns the return value of the unlocking coroutine.
  [[nodiscard]] auto reset_and_return()
  {
    assert(lkp_);
    return std::exchange(lkp_, nullptr)->unlock_and_return(this);
  }
  [[nodiscard]] auto reset_and_return(auto &&t)
  {
    assert(lkp_);
    return std::exchange(lkp_, nullptr)
        ->unlock_and_return(this, std::forward<decltype(t)>(t));
  }
};

} // namespace x23
