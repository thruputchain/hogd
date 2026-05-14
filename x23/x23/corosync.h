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
#include <concepts>
#include <coroutine>
#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <x23/async.h>
#include <x23/cleanup.h>
#include <x23/corotrace.h>
#include <x23/debug.h>
#include <x23/mvar.h>
#include <x23/threadpool.h>
#include <x23/utility.h>

namespace x23 {

namespace detail {

// Resume the coroutine in a ThreadPool.  If the pool argument is
// nullptr, resumes it synchronously in this thread.
void resume_in_another_thread(CoroHandle,
                              ThreadPool *pool = ThreadPool::get_thread_pool());

} // namespace detail

class CoroLock {
public:
  // Every coroutine that wants to acquire the lock needs a LockerState
  class LockerState {
  public:
    LockerState() noexcept = default;
    LockerState(LockerState &&) = delete;

  private:
    AtomicCoroHandle next_;
    friend CoroLock;
  };

  // Acquire the lock or suspend.  The return value must be
  // co_awaited.  You need a LockerState that will persist for the
  // whole time you have the lock.
  template<typename Self>
  [[nodiscard]] auto lock(this Self &self, LockerState *ls)
  {
    // LockerState can only be used for one lock at a time
    x23assert(!ls->next_);

    if constexpr (requires { self.block_while_inhibited(); })
      // Self is a CoroSharedLock
      return [](Self::AwaitAcquire await) -> Async<void> {
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
    if (std::coroutine_handle<> h = next(ls))
      h();
  }

  // Release the lock and schedule the next locker in a different
  // thread.
  void unlock_no_yield(LockerState *ls,
                       ThreadPool *pool = ThreadPool::get_thread_pool())
  {
    if (CoroHandle h = next(ls))
      detail::resume_in_another_thread(h, pool);
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
  struct AwaitAcquire {
    CoroLock &lk_;
    LockerState *next_;
    LockerState *prev_;

    AwaitAcquire(CoroLock &lk, LockerState *ls) : lk_(lk), next_(ls) {}
    bool await_ready() noexcept
    {
      return (prev_ = lk_.last_.exchange(next_)) == nullptr;
    }
    void await_suspend(CoroHandle h SLOC_DECL) const noexcept
    {
#if X23_CORO_TRACE
      if (PromiseBase *base = h.base())
        base->suspension_ = sloc;
#endif // X23_CORO_TRACE
      prev_->next_.put(h);
    }
    void await_resume() noexcept {}
  };

  template<typename T> struct AwaitReleaseAndReturn;
  template<typename T> struct AwaitReleaseAndReturnPreferCaller;

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
  [[nodiscard]] AwaitReleaseAndReturnPreferCaller<void>
  unlock_and_return_prefer_caller(LockerState *ls);
  template<typename T>
  [[nodiscard]] AwaitReleaseAndReturnPreferCaller<T>
  unlock_and_return_prefer_caller(LockerState *ls, T t);

  // True while lock is held
  bool busy() const { return last_.load(std::memory_order_acquire) != nullptr; }

private:
  // If last_ is NULL, the lock is free.  If last_ points to a
  // LockerState, then it belongs to the last coroutine in line to
  // acquire the lock, in which case a new coroutine can enqueue
  // itself by putting its own coroutine_handle into the last
  // LockerState and setting last_ to its own LockerState.
  std::atomic<LockerState *> last_{nullptr};

  CoroHandle next(LockerState *const ls)
  {
    LockerState *lscopy = ls;
    if (last_.compare_exchange_strong(lscopy, nullptr)) {
      x23assert(!ls->next_);
      return {};
    }
    while (!ls->next_)
      detail::pause_instruction();
    return ls->next_.take();
  }
};

template<> struct [[nodiscard]] CoroLock::AwaitReleaseAndReturn<void> {
  AwaitReleaseAndReturn(CoroLock &lk, LockerState *ls) : lk_(lk), ls_(ls) {}
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  bool await_ready() noexcept { return false; }
  template<typename Promise>
  auto await_suspend(std::coroutine_handle<Promise> moriturus) noexcept
  {
    Promise &p = moriturus.promise();
    p.return_void(); // Coroutine must return void
    return finish_suspend(moriturus);
  }
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
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
    detail::resume_in_another_thread(CoroHandle{next_coro, nullptr});
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

template<>
struct [[nodiscard]] CoroLock::AwaitReleaseAndReturnPreferCaller<void> {
  AwaitReleaseAndReturnPreferCaller(CoroLock &lk, LockerState *ls)
    : lk_(lk), ls_(ls)
  {}
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
    if (!next_locker)
      return next_coro ? next_coro : std::noop_coroutine();
    if (!next_coro)
      return next_locker;
    // Preserve bounded stack growth, but keep the awaiting caller on the
    // current thread so hot async return paths do not pay a thread-pool hop.
    detail::resume_in_another_thread(CoroHandle{next_locker, nullptr});
    return next_coro;
  }

private:
  CoroLock &lk_;
  LockerState *ls_;
};
template<typename T>
struct [[nodiscard]] CoroLock::AwaitReleaseAndReturnPreferCaller
  : CoroLock::AwaitReleaseAndReturnPreferCaller<void> {
  using super = CoroLock::AwaitReleaseAndReturnPreferCaller<void>;
  T t_;

  AwaitReleaseAndReturnPreferCaller(CoroLock &lk, LockerState *ls, T t)
    : super(lk, ls), t_(std::move(t))
  {}
  template<typename Promise>
  auto await_suspend(std::coroutine_handle<Promise> moriturus) noexcept
  {
    Promise &p = moriturus.promise();
    p.return_value(std::move(t_));
    return this->finish_suspend(moriturus);
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
inline CoroLock::AwaitReleaseAndReturnPreferCaller<void>
CoroLock::unlock_and_return_prefer_caller(CoroLock::LockerState *ls)
{
  return AwaitReleaseAndReturnPreferCaller<void>{*this, ls};
}
template<typename T>
inline CoroLock::AwaitReleaseAndReturnPreferCaller<T>
CoroLock::unlock_and_return_prefer_caller(LockerState *ls, T t)
{
  return AwaitReleaseAndReturnPreferCaller<T>{*this, ls, std::move(t)};
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
    x23assert(!lkp_);
    lkp_ = &lk;
    return lkp_->lock(this);
  }

  void reset()
  {
    if (lkp_)
      std::exchange(lkp_, nullptr)->unlock(this);
  }

  void reset_no_yield(ThreadPool *pool = ThreadPool::get_thread_pool())
  {
    if (lkp_)
      std::exchange(lkp_, nullptr)->unlock_no_yield(this, pool);
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
    x23assert(lkp_);
    return std::exchange(lkp_, nullptr)->unlock_and_return(this);
  }
  [[nodiscard]] auto reset_and_return(auto &&t)
  {
    x23assert(lkp_);
    return std::exchange(lkp_, nullptr)
        ->unlock_and_return(this, std::forward<decltype(t)>(t));
  }
  [[nodiscard]] auto reset_and_return_prefer_caller()
  {
    x23assert(lkp_);
    return std::exchange(lkp_, nullptr)->unlock_and_return_prefer_caller(this);
  }
  [[nodiscard]] auto reset_and_return_prefer_caller(auto &&t)
  {
    x23assert(lkp_);
    return std::exchange(lkp_, nullptr)
        ->unlock_and_return_prefer_caller(this, std::forward<decltype(t)>(t));
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

// Simple coroutine-based semaphore.
//
//    CoroSem sem(1);         // initial value of counter (defaults to 1)
//
//    co_await sem.acquire(); // returns counter times more than up
//
//    co_await sem.release(); // release semaphore
//
//    sem.release();          // can also release outside coroutine
class CoroSem {
public:
  CoroSem(int count = 1) : count_(count) {}
  ~CoroSem()
  {
    // If this fails you destroyed a CoroSem with waiters
    x23assert(!next_);
  }

  [[nodiscard]] auto acquire() noexcept { return Downer{*this}; }

  auto release()
  {
    std::unique_lock ul(lk_);
    if (Downer *head = next_) {
      if (!(next_ = head->next_)) // NOLINT(bugprone-assignment-in-if-condition)
        last_ = &next_;
      ul.unlock();
      head->h_();
    }
    else
      ++count_;
    return std::suspend_never{};
  }

private:
  struct Downer {
    CoroSem &s_;
    std::unique_lock<std::mutex> ul_;
    std::coroutine_handle<> h_;
    Downer *next_{};

    Downer(CoroSem &s) : s_(s) {}
    Downer(const Downer &) = delete;

    bool await_ready() noexcept
    {
      if (!ul_)
        ul_ = std::unique_lock(s_.lk_);
      if (!s_.count_)
        return false;
      --s_.count_;
      ul_.unlock();
      return true;
    }
    void await_suspend(auto h SLOC_DECL) noexcept
    {
      x23assert(ul_);
#if X23_CORO_TRACE
      if (PromiseBase *base = PromiseBase::get(h))
        base->suspension_ = sloc;
#endif // X23_CORO_TRACE
      h_ = h;
      *s_.last_ = this;
      s_.last_ = &next_;
      // release() before unlock(), not ul_.unlock().
      // unique_lock::unlock() does __m_->unlock() then __owns_=false, so
      // the __owns_ byte write lands AFTER the release on lk_.  Once lk_
      // is released a concurrent CoroSem::release() can pop this waiter
      // and resume the coroutine on another thread, and ~unique_lock on
      // that thread reads __owns_ — a data race with no happens-before
      // edge protecting it.  release()->unlock() reorders: __owns_=false
      // is sequenced before the release on lk_, so it rides along in the
      // release sequence and the resumer's acquire on lk_ makes it
      // observable.
      ul_.release()->unlock();
    }
    void await_resume() noexcept {}
  };

  std::mutex lk_;
  unsigned count_;
  Downer *next_{};
  Downer **last_{&next_};
};

// A CoroFutex is a way of suspending coroutines than can all
// subsequently be woken up at once via the broadcast() method.  The
// CoroFutex has an internal variable of type T (which you can
// atomically load, store, and read-modify-write), and allows you to
// wait only on condition that the internal variable have some
// specific value.
template<std::regular T = std::monostate> class CoroFutex {
public:
  using value_type = T;

  class [[nodiscard]] Awaiter {
    using Ptr = MVar<std::vector<ThreadPool::Job>>::Ptr;

  public:
    Awaiter(Ptr s) : waiters_(std::move(s)) {}
    bool await_ready() noexcept { return !waiters_; }
    void await_suspend(CoroHandle h SLOC_DECL) noexcept
    {
#if X23_CORO_TRACE
      if (PromiseBase *base = h.base())
        base->suspension_ = sloc;
#endif // X23_CORO_TRACE
      waiters_->emplace_back(h);
      waiters_.reset();
    }
    void await_resume() noexcept {}

  private:
    Ptr waiters_;
  };

  template<typename... Args>
  requires std::constructible_from<value_type, Args...>
  CoroFutex(Args &&...args) : val_(value_type(std::forward<Args>(args)...))
  {}
  ~CoroFutex() { x23assert(waiters_->empty()); }

  // Read internal value
  value_type load() noexcept
  {
    if constexpr (kAtomic)
      return val_.load(std::memory_order_relaxed);
    else {
      auto _p = waiters_.take();
      return val_;
    }
  }

  // Write internal value
  void store(value_type v) noexcept
  {
    auto _p = waiters_.take();
    if constexpr (kAtomic)
      val_.store(v, std::memory_order_relaxed);
    else
      val_ = v;
  }

  // Read-Modify-Write internal value by applying a lambda to a
  // std::reference_wrapper<T>.  For example:
  //
  //     futex.rmw([](auto& i) { ++i; });
  template<typename F>
  decltype(auto) rmw(F &&f)
      // If the requires fails, the argument needs to be by reference.
      // For instance, this would be a bug: rmw([](auto i) { ++i; })
      requires std::invocable<F, value_type &> &&
               (!std::invocable<F, value_type>)
  {
    auto _p = waiters_.take();
    if constexpr (kAtomic) {
      auto v = val_.load(std::memory_order_relaxed);
      Defer _d([&v, this] { val_.store(v, std::memory_order_relaxed); });
      return std::invoke(std::forward<F>(f), v);
    }
    else
      return std::invoke(std::forward<F>(f), val_);
  }

  // Suspend the coroutine until someone calls notify_all().
  Awaiter wait() { return Awaiter{waiters_.take()}; }

  // Suspend the coroutine until someone calls notify_all(), except
  // that if the internal value not old, then don't suspend.
  Awaiter wait(const T &old)
  {
    auto ptr = waiters_.take();
    if constexpr (kAtomic) {
      if (val_.load(std::memory_order_relaxed) != old)
        ptr.reset();
    }
    else if (val_ != old)
      ptr.reset();
    return Awaiter{std::move(ptr)};
  }

  // Wake all waiting threads (regardless of whether the current value
  // is the one they waited on).
  void broadcast(ThreadPool *pool = ThreadPool::get_thread_pool())
  {
    x23assert(pool);
    pool->go(waiters_.exchange({}));
  }

private:
  // If the atomic is always lock free, we can do reads without
  // exclusively caching the value.  Otherwise, std::atomic will
  // probably just hash to some global mutex, so we are better off
  // making val_ non-atomic and protecting reads with the same mutex
  // as waiters_.
  static constexpr bool kAtomic = std::atomic<T>::is_always_lock_free;

  [[no_unique_address]] std::conditional_t<kAtomic, std::atomic<value_type>,
                                           value_type> val_;
  MVar<std::vector<ThreadPool::Job>> waiters_;
};

// CoroFutex is used as a local inside coroutine frames — must not
// exceed default new alignment or GCC bug #104177 will SIGSEGV.
static_assert(alignof(CoroFutex<int>) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);

} // namespace x23
