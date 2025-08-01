// -*-C++-*-

// A CoroLock is a lock that lets only one coroutine at a time do
// something.  Unfortunately, because the C++ coroutine interface
// doesn't make all the information you want available all at once,
// the interface is a bit clunky and requires two lines of code to
// acquire a lock, one to declare a CoroGuard, another to acquire the
// lock:
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
// A CoroSharedLock is a CoroLock that also allows multiple threads to
// acquire the lock in shared mode.  It uses an RCU-like strategy in
// which the shared lock lasts until you suspend the coroutine.  As
// such, the CoroSharedLock must be tied to a particular ThreadPool.
//
//
// The basic strategy for CoroLock is that it contains a pointer to
// LockerState for last coroutine in line to acquire the lock, or NULL
// if the lock is free.  LockerState contains the coroutine_handle of
// the next coroutine to wake up.  When acquiring the lock, a
// coroutine C swaps itself into the last position pointer, then sets
// the coroutine_handle in LockerState of the coroutine that was
// previously last to point to C.  On release, a coroutine resumes the
// next coroutine.
//
// The structure is reminiscent of MCS spinlocks, though CoroLock
// suspends the coroutine, making it more comparable to a sleeping
// lock than a spinlock.
//
//
// A CoroSharedLock<> csl is an RCU-like type derived from CoroLock.
// You can lock it exclusively with a CoroGuard just like a CoroLock,
// except the exclusive lock operation quiesces the threadpool (which
// is expensive, but CoroSharedLock is optimized for acquiring shared
// locks and rarely needing the exclusive lock).
//
//    auto shared_holder = co_await csl.coro_lock_shared();
//
// Returns when there is no exclusive locker.  Since exclusive lockers
// quiesce the thread pool, the lock is effectively held in shared
// mode until the coroutine is suspended.  shared_holder panics if the
// coroutine has been suspended before it is destroyed.  (If it
// doesn't go out of scope, you can call shared_holder.reset() when
// you are done holding the shared lock.)

#pragma once

#include <atomic>
#include <cassert>

#include <x23/async.h>
#include <x23/corotrace.h>
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

  // Acquire the lock or suspend.  The return value must be
  // co_awaited.  You need a LockerState that will persist for the
  // whole time you have the lock.
  template<typename Self>
  [[nodiscard]] auto lock(this Self &self, LockerState *ls)
  {
    // LockerState can only be used for one lock at a time
    assert(ls->next.load(std::memory_order_relaxed) == nullptr);

    struct AwaitAcquire {
      Self &lk_;
      LockerState *next_;
      LockerState *prev_;

      AwaitAcquire(Self &lk, LockerState *ls) : lk_(lk), next_(ls) {}
      bool await_ready() { return !(prev_ = lk_.last_.exchange(next_)); }
      void await_suspend(std::coroutine_handle<> h SLOC_PARM)
      {
#if X23_CORO_TRACE
        corotrace.set_suspend(h, sloc, "CoroLock",
                              static_cast<const void *>(&lk_));
#endif // X23_CORO_TRACE
        prev_->next = h;
      }
      void await_resume() {}
    };

    if constexpr (requires { self.block_while_inhibited(); })
      // Self is a CoroSharedLock
      return [](AwaitAcquire await) -> Async<void> {
        co_await await;
        if (ThreadPool *pool = ThreadPool::get_thread_pool())
          co_await pool->async(&Self::block_while_inhibited, &await.lk_);
        else
          await.lk_.block_while_inhibited();
      }({self, ls});
    else
      // Self is a plain CoroLock
      return AwaitAcquire{self, ls};
  }

  // Release the lock and invoke the next coroutine waiting for the
  // lock before returning.
  void unlock(LockerState *ls)
  {
    if (std::coroutine_handle<> h = next(ls)) {
#if X23_CORO_TRACE
      corotrace.clear_suspend(h);
#endif // X23_CORO_TRACE
      h();
    }
  }

  Async<void> await_free()
  {
    LockerState ls;
    co_await lock(&ls);
    unlock(&ls);
    // Note: it's important that unlock resumes the next coroutine
    // before returning.  Hence, if there's a big chain of threads
    // each calling await_free() in a loop, then they will all return
    // from await_free after the last unlock has completed.
  }

private:
  template<typename T> struct AwaitReleaseAndReturn;

public:
  // Release the lock and immediately destroy the invoking coroutine,
  // resuming the next coroutine in line for the lock.  If present,
  // the second argument is passed to return_value in coroutines
  // returning a value.  The result of these functions must be
  // co_awaited, as in:
  //
  //     co_await lk.unlock_and_return(ls);
  [[nodiscard]] AwaitReleaseAndReturn<void> unlock_and_return(LockerState *ls);
  template<typename T>
  [[nodiscard]] AwaitReleaseAndReturn<T> unlock_and_return(LockerState *ls,
                                                           T t);

  // True while lock is held
  bool busy() const { return last_.load(std::memory_order_acquire); }

private:
  // If last_ is NULL, the lock is free.  If last_ points to a
  // LockerState, then it belongs to the last coroutine in line to
  // acquire the lock, in which case a new coroutine can enqueue
  // itself by putting its own atomic_coroutine_handle into the last
  // LockerState and setting last_ to its own LockerState.
  std::atomic<LockerState *> last_{nullptr};

  std::coroutine_handle<> next(LockerState *const ls)
  {
    LockerState *lscopy = ls;
    if (last_.compare_exchange_strong(lscopy, nullptr)) {
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
  AwaitReleaseAndReturn(CoroLock &lk, LockerState *ls) : lk_(lk), ls_(ls) {}
  bool await_ready() noexcept { return false; }
  template<typename Promise>
  auto await_suspend(std::coroutine_handle<Promise> moriturus) noexcept
  {
    Promise &p = moriturus.promise();
    p.return_void(); // Coroutine must return void
    return finish_suspend(moriturus);
  }
  void await_resume() noexcept { std::unreachable(); }

protected:
  // Returns NULL if there is no other coroutine to resume, otherwise
  // returns the coroutine.
  template<typename Promise>
  std::coroutine_handle<>
  finish_suspend(std::coroutine_handle<Promise> moriturus) noexcept
  {
    std::coroutine_handle<> next_locker = lk_.next(ls_);
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

private:
  CoroLock &lk_;
  LockerState *ls_;
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

// A CoroGuard is similar to a lock_guard for a CoroLock.  However,
// the syntax is different because a CoroLock requires LockerState to
// persist for the duration of the lock, so the CoroGuard holds the
// LockerState.  To ensure the state lives until the lock is released,
// the CoroGuard must be declared before the lock is acquired, as in:
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
  // destroys the invoking coroutine by simulating a co_return
  // statement.  Otherwise, if you are at the end of a coroutine and
  // release the lock normally before a co_return statement, the
  // invoking coroutine will not be destroyed until the next locker
  // suspends itself.  The version with an argument allows you to
  // specify a non-void return value for the unlocking coroutine, as
  // if the reset_and_return were followed by a "co_return t;".
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

// A CoroSharedLock is derived from a lock that provides a busy() test
// (e.g., CoroLock, TestLock, or SharedLock) but also provides a
// coro_shared_lock() method allowing coroutines to acquire the lock
// and suspend the coroutine rather than block if the lock is not
// available.  coro_shared_lock() returns a "holder" value you must
// keep in scope until you are done with the lock.  The shared lock is
// only good until the next time your coroutine is suspended, but as a
// result is heavily optimized within coroutines.  By contrast,
// CoroSharedLock is not optimized for exclusive locks, so is most
// suitable for situations where you acquire the shared lock all the
// time and rarely need an exclusive lock.
template<typename ExLock = CoroLock> class CoroSharedLock : public ExLock {
public:
  ThreadPool &pool_;

  template<typename... Args> requires std::constructible_from<ExLock, Args...>
  explicit CoroSharedLock(ThreadPool &pool, Args &&...args)
    : ExLock(std::forward<Args>(args)...), pool_(pool)
  {}

  void block_while_inhibited()
  {
    pool_.quiesce();
    if constexpr (requires { this->ExLock::block_while_inhibited(); })
      this->ExLock::block_while_inhibited();
  }

  // Note: there's nothing to do to unlock shared, because you give up
  // the lock as soon as the coroutine is suspended.  However, it's a
  // serious error to hold the lock across suspension points, so we
  // return a CrashIfSuspended.
  using CoroSharedHolder = ThreadPool::CrashIfSuspended;
  Async<CoroSharedHolder> coro_lock_shared()
  {
    if (ThreadPool::get_thread_pool() != &pool_)
      co_await pool_.bg();
    while (ExLock::busy()) {
      if constexpr (requires { this->await_free(); })
        co_await this->await_free();
      else
        co_await pool_.async(&ExLock::block_until_free, this);
    }
    co_return ThreadPool::CrashIfSuspended{};
  }
};

// A pointer that is good only until you suspend the current
// coroutine, which is the duration of the mutual exclusion provided
// by CoroSharedLock::coro_lock_shared.
template<typename T> class CoroTempPtr : public ThreadPool::CrashIfSuspended {
public:
  using value_type = T;

  CoroTempPtr(T *p = nullptr) noexcept : CrashIfSuspended(make_cis(p)), p_{p} {}
  CoroTempPtr(T *p, const ThreadPool::CrashIfSuspended &cs) noexcept
    : CrashIfSuspended(cs), p_(p)
  {}
  CoroTempPtr &operator=(T *p) noexcept
  {
    p_ = p;
    CrashIfSuspended::operator=(make_cis(p_));
    return *this;
  }

  T *get() const noexcept
  {
    check();
    return p_;
  }
  operator T &() const noexcept { return *get(); }
  T *operator->() const noexcept { return get(); }
  explicit operator bool() const noexcept { return p_; }
  void reset() noexcept
  {
    p_ = nullptr;
    CrashIfSuspended::reset();
  }

private:
  T *p_;
  [[no_unique_address]] ThreadPool::CrashIfSuspended cis_;

  static void make_cis(T *p)
  {
    return p ? CrashIfSuspended{} : CrashIfSuspended{nullptr};
  }
};

} // namespace x23
