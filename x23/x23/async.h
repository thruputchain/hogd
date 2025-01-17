// -*- C++ -*-

// Coroutine-based callback abstractions for event-driven code,
// providing the following principal abstractions:
//
// * Async<T> is an awaitable asynchronous task returning a T.  You
//   can co_await it from within a coroutine, invoke
//   Async<T>::operator()(Callback<T>) to get a callback with the
//   result, or just call Async<T>::operator()() to put the current
//   thread to sleep until you can get the result synchronously.
//
// A simple example:
//
//     Async<int>
//     get5()
//     {
//       co_return 5;
//     }
//
//     Async<int>
//     get10()
//     {
//       co_return (co_await get5()) + (co_await get5());
//     }
//
// * Awaitable<T> is an awaitable returning a T, but with nothing
//   initially connected to it.  You can think of it as a "future" in
//   the generic sense of the word (though unfortunately the terms
//   promise and future are confusingly overloaded with coroutines).
//
// * CoroCB<T> is a trivially-copyable callback-like function taking
//   an argument of type T.  It should fit in a register and be
//   efficient to pass by value.  A CoroCB<T> can be constructed from
//   an Awaitable<T>, allowing a coroutine to suspend itself and
//   register a resumption callback.  If you think of Awaitable<T> as
//   a "future," you can think of a CoroCB<T> as the corresponding
//   "promise" (though it is not the "promise_type" of any coroutine).
//
// A simple example:
//
//     void
//     register_callback(CoroCB<int> cb)
//     {
//       cb(5);
//     }
//
//     Async<int>
//     twice_cbval()
//     {
//       Awaitable<int> a;
//       register_callback(a);
//       int n = co_await a;
//       co_return 2*n;
//     }
//
//     int
//     main()
//     {
//       twice_cbval()([](int n) {
//         assert(n == 10);
//       });
//     }
//
// * Any funtion returning the vacuous type Detached can be a
//   coroutine that uses co_await and co_return (co_returing void),
//   but cannot itself be co_awaited.  This is useful for completing
//   work in the background.
//
// * Any function returning void whose final argument is a
//   Callback-like type (meaning Callback<T> or any other movable type
//   for which callback_traits has been specialized) can be a
//   co_routine if it returns via "co_return t" for some t of type T.
//
// * If a coroutine's return_value function expects type
//   std::expected<T, E> and the coroutine has an object r of type
//   std::expected<R, E> with the same error type, the coroutine can
//   extract the value of r with "co_await r".  If r contains an
//   error, the coroutine will immediately return it.  For example:
//
//     Async<std::expected<Buf,Errno>> read_record();
//     std::expected<Object,Errno> parse(Buf);
//
//     Async<std::expected<Object,Errno>>
//     get_object()
//     {
//       buf = co_await co_await read_record();
//       co_return co_await parse(buf);
//     }

#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <semaphore>
#include <tuple>
#include <utility>

#if HAVE_BUILTIN_ARM_YIELD
#include <arm_acle.h>
#endif // HAVE_BUILTIN_ARM_YIELD

#include <x23/cleanup.h>
#include <x23/config.h>
#include <x23/utility.h>

namespace x23 {

// A Callback is just a std::function of 0-1 arguments, but we use a
// distinguished type to ensure template specializations don't affect
// std::functions in general.  Any function returning void whose final
// argument is of type Callback<T> can be a coroutine if it returns
// with co_return.  However, such a coroutine must never actually use
// its final argument, as the value will be moved into the promise
// object.
template<typename R> struct Callback : std::function<void(R)> {
  using std::function<void(R)>::function;
};
template<> struct Callback<void> : std::function<void()> {
  using std::function<void()>::function;
};
template<typename F> requires requires(F f) { f(); }
Callback(F) -> Callback<void>;
template<typename F> requires requires(F f) {
  requires FnTraits<F>::num_args == 1;
  f(std::declval<typename FnTraits<F>::template arg_type<0>>());
}
Callback(F) -> Callback<typename FnTraits<F>::template arg_type<0>>;

template<typename CB> struct callback_traits;
template<typename R> struct callback_traits<Callback<R>> {
  using argument_type = R;
};

namespace detail {

template<typename CB>
concept is_callback_like =
    requires { typename callback_traits<CB>::argument_type; };

template<is_callback_like CB>
using CallbackArgType = typename callback_traits<CB>::argument_type;

template<typename CB, typename T>
concept is_callback_of =
    is_callback_like<CB> && std::same_as<CallbackArgType<CB>, T>;

template<is_callback_like CB, typename R = CallbackArgType<CB>>
struct CallbackPromiseReturn {
  using argument_type = R;
  CB cb_;
  void return_value(decays_to<R> auto &&r)
  {
    cb_(std::forward<decltype(r)>(r));
  }
};
template<typename CB> struct CallbackPromiseReturn<CB, void> {
  using argument_type = void;
  CB cb_;
  void return_void() { cb_(); }
};

// This type allows you to co_return from a function whose final
// argument is a callback.
template<is_callback_like CB>
struct CallbackPromise : CallbackPromiseReturn<CB> {
  // Note the coroutine should not touch its final argument (the
  // callback), so we can move it into ourselves.  (The arguments to a
  // promise constructor are initialized from the already initialized
  // arguments of the coroutine.)
  CallbackPromise(auto &&...args) requires (sizeof...(args) > 0)
  {
    this->cb_ = std::move(last_arg(std::forward<decltype(args)>(args)...));
  }
  CallbackPromise(CallbackPromise &&) = delete;

  void get_return_object() const noexcept {}
  std::suspend_never initial_suspend() const noexcept { return {}; }
  std::suspend_never final_suspend() const noexcept { return {}; }
  void unhandled_exception() noexcept { std::terminate(); }
};

} // namespace detail
} // namespace x23

template<typename... Args>
requires x23::detail::is_callback_like<x23::LastType<Args...>>
struct std::coroutine_traits<void, Args...> {
  using promise_type = x23::detail::CallbackPromise<x23::LastType<Args...>>;
};

namespace x23 {

using atomic_coroutine_handle = std::atomic<std::coroutine_handle<>>;

namespace detail {

// Supertype of Awaitable so Awaitable can be specialized for void
// without duplicating a lot of code.
template<typename R> class AwaitableImpl {
public:
  using resume_type = R;

  AwaitableImpl(AwaitableImpl &&) = delete;

  //
  // Functions for use on the callback/callee/"promise" side
  //

  // Call once to indicate a result is pending.  This is to catch bugs
  // where an active Async is destroyed without co_awaiting the
  // result.  Note launch is already called by the constructors of
  // CoroCB and Async, so you shouldn't need to call it unless you are
  // introducing a new type.
  void launch()
  {
    if (state_ == kUnused)
      state_ = kPending;
    else
      assert(state_ == kPending);
  }
  void set_result(references<R> auto &&...r)
      requires (sizeof...(r) == !std::is_void_v<R>)
  {
    do_set_result(std::forward<decltype(r)>(r)...);
    non_null_caller().resume();
  }
  void set_exception(std::exception_ptr p)
  {
    do_set_exception(std::move(p));
    non_null_caller().resume();
  }

  std::suspend_never initial_suspend() const noexcept { return {}; }
  void unhandled_exception() { do_set_exception(std::current_exception()); }
  struct TailCoro {
    std::coroutine_handle<> caller_;
    bool await_ready() const noexcept { return false; }
    auto await_suspend(std::coroutine_handle<>) const noexcept
    {
      return caller_;
    }
    void await_resume() const noexcept { std::unreachable(); }
  };
  TailCoro final_suspend() noexcept { return {non_null_caller()}; }

  //
  // Functions for use on the caller/awaiter/"future" side
  //

  bool await_ready() noexcept
  {
    // If the test_and_set returns true, the callee already wrote
    // result_ or exception_, so use acquire semantics to access it.
    return resume_lock_.test_and_set(std::memory_order_acquire);
  }
  void await_suspend(std::coroutine_handle<> h) noexcept
  {
    caller_.store(h, std::memory_order_release);
    // Once we store a non-NULL coroutine_handle, the callee may
    // resume us and destroy the Awaitable at any time, so we must no
    // longer touch *this.  Release semantics are to ensure any memory
    // accesses other than result_ are properly sequenced between the
    // suspend and resume.
  }
  auto await_resume()
  {
    if (state_ == kResult) [[likely]] {
      state_ = kConsumed;
      Defer _c{[this] { std::destroy_at(&result_); }};
      if constexpr (std::is_void_v<R>)
        return;
      else
        return std::move(result_);
    }
    assert(state_ == kException);
    state_ = kConsumed;
    Defer _c{[this] { std::destroy_at(&exception_); }};
    std::rethrow_exception(std::move(exception_));
  }

  //
  // Low-level functions you shouldn't need to call unless you are
  // implementing CoroCB-like classes...
  //
  void do_set_result(references<R> auto &&...r)
      requires (sizeof...(r) == !std::is_void_v<R>)
  {
    assert(state_ == kPending);
    std::construct_at(&result_, std::forward<decltype(r)>(r)...);
    state_ = kResult;
  }
  void do_set_exception(std::exception_ptr p)
  {
    assert(state_ == kPending);
    std::construct_at(&exception_, std::move(p));
    state_ = kException;
  }
  std::coroutine_handle<> caller()
  {
    if (!resume_lock_.test_and_set(std::memory_order_release))
      // The caller is not suspending, so don't resume it.  Use
      // release semantics so a non-suspending caller can subsequently
      // access result_ or exception_.
      //
      // Note: Once we've successfully set resume_lock_ (telling the
      // caller not to suspend), the caller may delete the
      // AwaitableImpl at any time, so we must no longer access *this.
      return nullptr;
    // The caller is suspending and will store its handle in caller_
    // and not destroy the AwaitableImpl until we resume it.  We are
    // resuming the coroutine in the same thread that ran the callee,
    // so there are no memory ordering issues with result_.  On the
    // other hand, we might need to ensure the order of access to
    // other coroutine state or even global variables on either side
    // of the co_await statement, e.g.:
    //
    //      global_int = 1;      // <-- line 1
    //      co_await my_awaiter;
    //      use(global_int);     // <-- line 2
    //
    // Since line 2 will run in this thread and line 1 ran in a
    // different thread, we use acquire semantics to ensure line 1
    // happens before line 2.
    std::coroutine_handle<> h = caller_.load(std::memory_order_acquire);
    if (h) [[likely]]
      return h;
    // h could be NULL if the callee suspended but then was resumed
    // and completed in another thread in the brief instant between
    // when the caller invoked await_ready() and await_suspend().
    // In that case, we wait for the caller to set caller_.
    do
      pause_instruction();
    while (!(h = caller_.load(std::memory_order_relaxed)));
    caller_.load(std::memory_order_acquire);
    return h;
  }

protected:
  AwaitableImpl() noexcept {}
  ~AwaitableImpl()
  {
    // You cannot discard an Awaitable without co_awaiting it, unless
    // it has no work associated with it, meaning it's not an Async
    // and you haven't turned it into a CoroCB.  Otherwise, you could
    // destroy the Awaitable while it's in use by another thread.
    assert(state_ == kUnused || state_ == kConsumed);
  }

private:
  // Initially NULL, then the caller's handle if and when it decides
  // to suspend itself (which will happen when the caller calls
  // co_await before the callee returns a result).
  atomic_coroutine_handle caller_{nullptr};

  // Both the coroutine and the resumer test_and_set resume_lock_.
  // Whichever of the two finds the flag already set (and hence was
  // second to call test_and_set) is responsible for continuing the
  // coroutine.  Specifically, either the caller continues the
  // coroutine by not suspending or the resumer continues the
  // coroutine by resuming it.
  std::atomic_flag resume_lock_ = ATOMIC_FLAG_INIT;

  enum State : std::uint8_t {
    kUnused = 0,
    kPending = 1,
    kResult = 2,
    kException = 3,
    kConsumed = 4,
  };
  State state_{kUnused};
  union {
    std::conditional_t<std::is_void_v<R>, std::tuple<>, R> result_;
    std::exception_ptr exception_;
  };

  std::coroutine_handle<> non_null_caller()
  {
    if (std::coroutine_handle<> h = caller())
      return h;
    return std::noop_coroutine();
  }
};

} // namespace detail

// Simple way for a coroutine to get its own handle:
//
//    auto myhandle = co_await GetCoro{};
template<typename Promise = void> struct [[nodiscard]] GetCoro {
  [[nodiscard]] constexpr GetCoro() noexcept = default;
  std::coroutine_handle<Promise> h_;
  bool await_ready() const noexcept { return false; }
  bool await_suspend(std::coroutine_handle<Promise> h) noexcept
  {
    h_ = h;
    return false;
  }
  std::coroutine_handle<Promise> await_resume() const noexcept { return h_; }
};

// Data shared between a suspended caller coroutine that is
// co_awaiting a result of type R and whatever callback or callee
// coroutine is going to supply that result.
template<typename R> struct Awaitable : detail::AwaitableImpl<R> {
  void return_value(references<R> auto &&r)
  {
    this->do_set_result(std::forward<decltype(r)>(r));
  }
};
template<> struct Awaitable<void> : detail::AwaitableImpl<void> {
  void return_void() { do_set_result(); }
};

// A CoroCB<R> is a callback-like type associated with an Awaitable<R>
// that resumes whatever co_routine is co_awaiting the Awaitable<R>.
// The type is trivially copyable and should fit in a register.
//
// Important: once you have constructed a CoroCB from an Awaitable,
// you must invoke the CoroCB and you must must co_await the
// Awaitable.  Destroying the Awaitable and then invoking the CoroCB
// would yield undefined behavior.  Hence, there's an assertion
// failure when you destroy an Awaitable without first co_awaiting it
// (unless you never constructed a CoroCB from the Awaitable).
template<typename R> struct CoroCB {
  Awaitable<R> *resume_{};

  CoroCB() noexcept = default;
  CoroCB(Awaitable<R> &rp) noexcept : resume_(&rp) { resume_->launch(); }
  CoroCB(std::nullptr_t) {}

  explicit operator bool() const noexcept { return resume_; }
  template<typename... T>
  void operator()(T &&...t) const
      requires requires { resume_->set_result(std::forward<T>(t)...); }
  {
    resume_->set_result(std::forward<T>(t)...);
  }
  void raise(std::exception_ptr p) const { resume_->set_exception(p); }

  // Low-level function to set the result without resuming the
  // coroutine.  This can be useful if you want to set the result
  // immediately but resume the coroutine in a different thread.  You
  // must resume the returned std::coroutine_handle<> if and only if
  // it is non-NULL.  The function will return a NULL handle if the
  // caller has not yet co_awaited the Awaitable, in which case it
  // will simply take the result without ever suspending itself.
  template<typename... T>
  std::coroutine_handle<> set_result_without_resuming(T &&...t) const
      requires requires { resume_->set_result(std::forward<T>(t)...); }
  {
    resume_->do_set_result(std::forward<T>(t)...);
    return resume_->caller();
  }
};

template<typename R> struct callback_traits<CoroCB<R>> {
  using argument_type = R;
};

// An asynchronous result.  A function returning Async<R> can be
// implemented as a coroutine.  A coroutine with an Async<R> a can
// "co_await a" to get the result.  A non-coroutine can invoke an
// Async with a callback as "a([](R r) { ... })" to be called back
// with the result, or can sleep and wait for the result by invoking
// it with no arguments "a()".
//
// An Async<T> should fit in a register, so can cheaply be moved,
// though it cannot be copied since it destroys the coroutine in its
// destructor.  A non-coroutine function can return an Async by
// passing back the result of another function returning an Async, or
// can create a trivial Async that returns a specific value by calling
// async_return(R).
//
// Note that you must get the result of an Async before destroying it,
// as the result is likely being produced in a different thread that
// is continuing to access the coroutine state that gets destroyed by
// Async's destructor.
template<typename R> struct [[nodiscard]] Async {
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;
  struct promise_type : Awaitable<R> {
    Async get_return_object()
    {
      return Async{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
  };

  [[nodiscard]] Async(Async &&other) noexcept
    : callee_(std::exchange(other.callee_, nullptr))
  {}
  ~Async()
  {
    if (callee_)
      callee_.destroy();
  }

  // Since Asyncs can be moved, all the state is in the promise object.
  decltype(auto) operator co_await() { return callee_.promise(); }

  // Run Async to completion and make a callback with the result.
  void operator()(detail::is_callback_of<R> auto cb)
  {
    // To defer destroying callee_, move Async into this coroutine
    Async self = std::move(*this);
    co_return co_await self;
  }
  template<std::convertible_to<Callback<R>> CB>
  requires (!detail::is_callback_of<CB, R>)
  void operator()(CB &&cb)
  {
    (*this)(Callback<R>{std::forward<CB>(cb)});
  }

  // Return the Async value synchronously, pausing the current thread
  // until the result is ready.
  R operator()()
  {
    std::binary_semaphore wait(0);
    if constexpr (std::is_void_v<R>) {
      (*this)([&wait] { wait.release(); });
      wait.acquire();
    }
    else {
      std::optional<R> res;
      (*this)([&res, &wait](R r) {
        res.emplace(std::move(r));
        wait.release();
      });
      wait.acquire();
      return *res;
    }
  }

private:
  handle_type callee_;
  Async(handle_type h) : callee_(h) { h.promise().launch(); }
};

// An Async that immediately returns its argument.  Useful in
// non-coroutine functions that return Async by invoking a coroutine,
// but where some cases don't need to invoke a coroutine.  E.g.,:
//
//   Async<Buf> get_record(int fd);  // some coroutine
//
//   Async<Buf>
//   read_data(int fd)
//   {
//     if (fd = -1)
//       return async_return(Buf{});
//     else
//       return get_record(fd);
//   }
template<typename R>
Async<R>
async_return(R &&r)
{
  co_return std::forward<R>(r);
}

// Generate an Async and matching CoroCB.  This is for use in
// low-level non-coroutine functions that want to return an Async for
// coroutines to co_await.
template<typename R>
std::pair<Async<R>, CoroCB<R>>
make_async()
{
  CoroCB<R> cb;
  Async<R> async = [&cb] -> Async<R> {
    auto h = co_await GetCoro<typename Async<R>::promise_type>{};
    cb = h.promise();
    co_await std::suspend_always{};
    std::unreachable();
  }();
  return std::pair{std::move(async), cb};
}

template<typename R>
auto
async_map(auto f, Async<R> a) -> Async<decltype(f(std::declval<R>()))>
{
  co_return f(co_await a);
}
auto
async_map(auto f, Async<void> a) -> Async<decltype(f())>
{
  co_await a;
  co_return f();
}

// A funciton that returns Detached is a detached coroutine that
// cannot be awaited.  The coroutine destroys itself upon co_return;
struct Detached {
  struct promise_type {
    Detached get_return_object() const noexcept { return {}; }
    std::suspend_never initial_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept { std::terminate(); }
    std::suspend_never final_suspend() const noexcept { return {}; }
  };

  Detached(Detached &&) noexcept = delete;

private:
  // Make constructor private to flag an error in case a function
  // returns Detached without using coroutine operations (i.e.,
  // co_await or co_return) to avoid weird surprises.
  Detached() noexcept = default;
};

// A concept that's 99% likely to determine whether a type is
// awaitable.  You could have types that are awaitable but only with
// specific promise types not including Detached::promise_type, in
// which case this will fail even if the type is awaitable in other
// types of coroutine.
template<typename A>
concept is_awaitable =
    requires(A a, std::coroutine_handle<Detached::promise_type> h) {
      { a.await_ready() } -> std::same_as<bool>;
      a.await_suspend(h);
      a.await_resume();
    };

namespace detail {

// If final_suspend doesn't suspend a coroutine, then the coroutine
// state is implicitly destroyed.  We emulate this behavior.  In the
// event that final_suspend().await_suspend() returns a
// coroutine_handle to jump to, we also return this handle.
template<typename Promise>
auto // either void or coroutine_handle<>
emulate_final_suspend(std::coroutine_handle<Promise> h)
{
  auto a = h.promise().final_suspend();
  using suspend_type = decltype(a.await_suspend(h));

  if constexpr (std::is_void_v<suspend_type>) {
    if (a.await_ready())
      h.destroy();
    else
      a.await_suspend(h);
  }
  else if constexpr (std::same_as<suspend_type, bool>) {
    if (a.await_ready() || !a.await_suspend(h))
      h.destroy();
  }
  else {
    std::coroutine_handle<> ret;
    if (a.await_ready()) {
      h.destroy();
      ret = std::noop_coroutine();
    }
    else
      ret = a.await_suspend(h);
    return ret;
  }
}

template<typename T, typename E> struct ExpectAwait {
  std::expected<T, E> r_;

  bool await_ready() { return r_.has_value(); }
  template<typename P> auto await_suspend(std::coroutine_handle<P> h)
  {
    h.promise().return_value(std::unexpected(r_.error()));
    return emulate_final_suspend(h);
  }
  T await_resume() { return std::move(*r_); }
};

} // namespace detail
} // namespace x23

template<typename T, typename E>
x23::detail::ExpectAwait<T, E> operator co_await(std::expected<T, E> &&r)
{
  return x23::detail::ExpectAwait{std::move(r)};
}
template<typename T, typename E>
x23::detail::ExpectAwait<T, E> operator co_await(const std::expected<T, E> &r)
{
  return x23::detail::ExpectAwait{r};
}
