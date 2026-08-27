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
//   (Note: the promise_type of an Async is a subtype of Awaitable,
//   and the Async behaves like a unique reference to the Awaitable.)
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
// * The function make_async<R>() creates a matched
//   std::pair<Async<R>, CoroCB<R>>.  This can be used for low-level
//   event harness code:  return the Async for the invoking coroutine
//   to co_await, and save the CoroCB to be invoked when the event
//   fires.
//
// * Any function returning the vacuous type Detached can be a
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
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <semaphore>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#if HAVE_BUILTIN_ARM_YIELD
#include <arm_acle.h>
#endif // HAVE_BUILTIN_ARM_YIELD

#include <x23/cleanup.h>
#include <x23/config.h>
#include <x23/corotrace.h>
#include <x23/debug.h>
#include <x23/typename.h>
#include <x23/utility.h>

namespace x23 {

// This is an otherwise useless return value that resumes a coroutine
// on its destruction.  It's useful in cases where a coroutine could
// possibly suspend itself and resume itself in another thread before
// the coroutine function returns.
struct CoResume {
  constexpr CoResume() noexcept = default;
  explicit CoResume(std::coroutine_handle<> h) noexcept : h_(h) {}
  CoResume(CoResume &&other) noexcept : h_(std::exchange(other.h_, {})) {}
  ~CoResume()
  {
    if (h_)
      h_();
  }

  CoResume &operator=(CoResume &&other) noexcept
  {
    if (auto oldh = std::exchange(h_, std::exchange(other.h_, {})))
      oldh();
    return *this;
  }

private:
  std::coroutine_handle<> h_;
};

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
  f(declval<typename FnTraits<F>::template arg_type<0>>());
}
Callback(F) -> Callback<typename FnTraits<F>::template arg_type<0>>;

template<typename CB> struct callback_traits;
template<typename R> struct callback_traits<Callback<R>> {
  using argument_type = R;
};

namespace detail {

template<typename CB>
concept is_callback_like =
    requires { typename callback_traits<std::decay_t<CB>>::argument_type; };

template<is_callback_like CB>
using CallbackArgType =
    typename callback_traits<std::decay_t<CB>>::argument_type;

template<typename CB, typename T>
concept is_callback_of =
    is_callback_like<CB> && std::same_as<CallbackArgType<CB>, T>;

// This is a mix-in class that somewhat abstracts the difference
// between coroutines that return void and those that return a value,
// by, in either case, calling a method do_return() in the subtype.
template<typename T> struct CoroDoReturn : PromiseBase {
  static constexpr size_t kReturnValue = 1;
  using PromiseBase::PromiseBase;
  template<std::convertible_to<T> R>
  void return_value(this auto &self, R &&v) noexcept
  {
    self.do_return(std::forward<R>(v));
  }
};
template<> struct CoroDoReturn<void> : PromiseBase {
  static constexpr size_t kReturnValue = 0;
  using PromiseBase::PromiseBase;
  void return_void(this auto &self) noexcept { self.do_return(); }
};

// This type allows you to co_return from a function whose final
// argument is a callback.
template<typename Return, typename... Args>
requires is_callback_like<LastType<Args...>>
struct CallbackPromise : CoroDoReturn<CallbackArgType<LastType<Args...>>> {
  using CB = LastType<Args...>;
  using argument_type = CallbackArgType<CB>;
  using super = CoroDoReturn<argument_type>;
  using super::kReturnValue;
  // We would like to return CoResume from get_return_object() always.
  // This works fine in gcc, but causes an internal compiler error
  // with clang 22.  The C++ spec is a bit ambiguous on this
  // matter--if get_return_object() is void the coroutine function
  // must be void, while the converse isn't explicitly specified.
  static constexpr bool kInitialSuspend = std::same_as<Return, CoResume>;
  CB cb_;

  // Note the coroutine should not touch its final argument (the
  // callback), so we can move it into ourselves.  (The arguments to a
  // promise constructor are initialized from the already initialized
  // arguments of the coroutine.)
  CallbackPromise(Args &...args SLOC_DECL)
    : super(SLOC_VAR),
      cb_(std::move(last_arg(std::forward<decltype(args)>(args)...)))
  {}
  CallbackPromise(const CallbackPromise &) = delete;

  // If you hit this warning, you are taking a function that takes a
  // callback and returns void and implementing the function as a
  // coroutine.  In such cases, you need to ensure coroutine doesn't
  // suspend itself, then resume, return, and destroy itself in a
  // different thread all before the coroutine ramp function has
  // returned in the original thread.  If you can't ensure that, there
  // will be a use-after-free error on the resumption index in the
  // coroutine object, which is touched by the ramp function.  A safer
  // approach is for the function taking a callback to return
  // x23::CoResume, in which case we can resume the coroutine after
  // the ramp function as returned.
  template<bool B = kInitialSuspend> requires (B == kInitialSuspend && !B)
  [[deprecated("return CoResume rather than void from coroutine callback")]]
  static void get_return_object() noexcept
  {}

  template<bool B = kInitialSuspend> requires (B == kInitialSuspend && B)
  CoResume get_return_object() noexcept
  {
    return CoResume{
        std::coroutine_handle<CallbackPromise>::from_promise(*this)};
  }

  auto initial_suspend() const noexcept
  {
    if constexpr (kInitialSuspend)
      return std::suspend_always{};
    else
      return std::suspend_never{};
  }

  void do_return(auto &&...v) requires (sizeof...(v) == kReturnValue)
  {
    std::invoke(std::move(cb_), std::forward<decltype(v)>(v)...);
  }
  std::suspend_never final_suspend() const noexcept { return {}; }
  void unhandled_exception() noexcept { trace_unhandled_exception(this); }
};

} // namespace detail
} // namespace x23

template<x23::is_one_of<x23::CoResume, void> R, typename... Args>
requires x23::detail::is_callback_like<x23::LastType<Args...>>
struct std::coroutine_traits<R, Args...> {
  using promise_type = x23::detail::CallbackPromise<R, Args...>;
};

template<x23::is_one_of<x23::CoResume, void> R, typename... Args>
requires x23::has_location<Args...> &&
         x23::detail::is_callback_like<
             x23::TypeNum<sizeof...(Args) - 2, Args...>>
struct std::coroutine_traits<R, Args...> {
  using promise_type = decltype([]<std::size_t... N>(std::index_sequence<N...>)
                                    -> x23::detail::CallbackPromise<
                                        R, x23::TypeNum<N, Args...>...> {
    throw;
  }(std::make_index_sequence<sizeof...(Args) - 1>{}));
};

namespace x23 {

// A non-moveable Awaitable type that can be implicitly converted to a
// CoroCB.
template<typename R> class Awaitable : public detail::CoroDoReturn<R> {
public:
  using resume_type = R;
  using super = detail::CoroDoReturn<R>;
  using super::kReturnValue;

  Awaitable(source_location sloc = source_location::current()) : super(sloc) {}
  Awaitable(const Awaitable &) = delete;
  ~Awaitable()
  {
    // You cannot discard an Awaitable without co_awaiting it, unless
    // it has no work associated with it, meaning it's not an Async
    // and you haven't turned it into a CoroCB.  Otherwise, you could
    // destroy the Awaitable while it's in use by another thread.
#ifndef NDEBUG
    if (auto fl = flags()) {
      auto tr = trace(this, 4);
      if (auto sz = tr.size())
        // Trim newline as panic will add one
        tr.resize(sz - 1);
      panic("Premature destruction of a running Awaitable or Async"
            " [flags = {:#x}]\n  Trace of destroyed Awaitable:\n{}",
            fl, tr);
    }
#endif // !NDEBUG
  }

  //
  // Functions for use on the callee/"promise" side
  //

  // Call once to indicate a result is pending.  This is to catch bugs
  // where an active Async is destroyed without co_awaiting the
  // result.  Note launch is already called by the constructors of
  // CoroCB and Async, so you shouldn't need to call it unless you are
  // introducing a new type.
  void launch()
  {
    x23assert(!(flags() & ~kRunning));
    flags_.store(kRunning, std::memory_order_relaxed);
  }
  void set_result(references<R> auto &&...r)
      requires (sizeof...(r) == kReturnValue)
  {
    do_return(std::forward<decltype(r)>(r)...);
    non_null_caller().resume();
  }
  void set_exception(std::exception_ptr p)
  {
    do_exception(std::move(p));
    non_null_caller().resume();
  }

  std::suspend_never initial_suspend() const noexcept { return {}; }
  void unhandled_exception() noexcept
  {
    do_exception(std::current_exception());
  }
  struct TailCoro {
    Awaitable *self_;
    bool await_ready() const noexcept { return false; }
    // Publish kReturning here in await_suspend, not in
    // final_suspend.  The compiler writes the suspend-point index to
    // the coroutine frame between final_suspend() returning and
    // await_suspend() being called.  Publishing kReturning earlier
    // lets the caller destroy the frame while that write is
    // in-flight.
    auto await_suspend(std::coroutine_handle<>) const noexcept
    {
      return self_->non_null_caller();
    }
    void await_resume() const noexcept { std::unreachable(); }
  };
  TailCoro final_suspend() noexcept { return {this}; }

  //
  // Functions for use on the caller/awaiter/"future" side
  //
  bool await_ready() noexcept
  {
    return (flags_.load(std::memory_order_acquire) & kReturning) != 0U;
  }

  bool await_suspend(auto h SLOC_DECL) noexcept
  {
#if X23_CORO_TRACE
    if (auto base = PromiseBase::get(h)) {
      base->suspension_ = sloc;
      this->waiter_ = base;
    }
#endif // X23_CORO_TRACE
    caller_ = h;
    unsigned oldflags = flags_.fetch_or(kAwaiting, std::memory_order_acq_rel);

    // Once we set kAwaiting, the callee may resume us and destroy the
    // Awaitable at any time, so we must no longer touch *this.
    return !(oldflags & kReturning);
  }
  auto await_resume()
  {
    unsigned f = flags_.exchange(0, std::memory_order_relaxed);
    if (f & kResult) [[likely]] {
      Defer _c{[this] { std::destroy_at(&result_); }};
      if constexpr (kReturnValue)
        return std::move(result_);
      else
        return;
    }
    x23assert(f & kException);
    Defer _c{[this] { std::destroy_at(&exception_); }};
    std::rethrow_exception(std::move(exception_));
  }

  //
  // Low-level functions you shouldn't need to call unless you are
  // implementing CoroCB-like classes...
  //
  void do_return(references<R> auto &&...r)
      requires (sizeof...(r) == kReturnValue)
  {
    x23assert((flags() & (kRunning | kResult | kException)) == kRunning);
    std::construct_at(&result_, std::forward<decltype(r)>(r)...);
    flags_.fetch_or(kResult, std::memory_order_relaxed);
  }
  void do_exception(std::exception_ptr p)
  {
    x23assert((flags() & (kRunning | kResult | kException)) == kRunning);
    std::construct_at(&exception_, std::move(p));
    flags_.fetch_or(kException, std::memory_order_relaxed);
  }
  std::coroutine_handle<> caller()
  {
    unsigned oldflags = flags_.fetch_or(kReturning, std::memory_order_acq_rel);
    x23assert(oldflags & (kResult | kException));
    if (oldflags & kAwaiting) {
      return caller_;
    }
    // At this point no longer safe to touch this, as caller might
    // destroy coroutine.
    return nullptr;
  }

private:
  // Caller's handle after it decides to suspend itself (if the result
  // is not already ready).
  std::coroutine_handle<> caller_;

  // result_ has been constructed:
  static constexpr unsigned kResult = 1U << 0;
  // exception_ has been constructed:
  static constexpr unsigned kException = 1U << 1;
  // a coroutine is running to produce the result:
  static constexpr unsigned kRunning = 1U << 2;
  // a coroutine is returning the result or excaption:
  static constexpr unsigned kReturning = 1U << 3;
  // a caller is awaiting the result:
  static constexpr unsigned kAwaiting = 1U << 4;

  // Bitwise or of above flags.  Note the caller sets kAwaiting while
  // the callee coroutine sets kReturning.  If the coroutine sets
  // kReturning before kAwaiting was set, then the caller does not
  // suspend itself.
  std::atomic_uint flags_{0};

  union {
    std::conditional_t<std::is_void_v<R>, std::tuple<>, R> result_;
    std::exception_ptr exception_;
  };

  unsigned flags() const noexcept
  {
    return flags_.load(std::memory_order_relaxed);
  }

  std::coroutine_handle<> non_null_caller()
  {
    if (std::coroutine_handle<> h = caller())
      return h;
    return std::noop_coroutine();
  }
};

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
  CoroHandle set_result_without_resuming(T &&...t) const
      requires requires { resume_->set_result(std::forward<T>(t)...); }
  {
    resume_->do_return(std::forward<T>(t)...);
    if (std::coroutine_handle<> h = resume_->caller())
      return {h, resume_->waiter()};
    return {};
  }
};

template<typename R> struct callback_traits<CoroCB<R>> {
  using argument_type = R;
};

namespace detail {
struct SuspendedCoroPromise : PromiseBase {
  template<typename... Args> requires has_location<Args...>
  SuspendedCoroPromise(Args &&...args) : PromiseBase(extract_location(args...))
  {}
  SuspendedCoroPromise(SLOC_DECL0) : PromiseBase(SLOC_VAR) {}
  std::coroutine_handle<SuspendedCoroPromise> get_return_object()
  {
    return std::coroutine_handle<SuspendedCoroPromise>::from_promise(*this);
  }
  static std::suspend_always initial_suspend() noexcept { return {}; }
  void return_void() const noexcept {}
  void unhandled_exception() noexcept { trace_unhandled_exception(this); }
  static std::suspend_never final_suspend() noexcept { return {}; }
};
} // namespace detail

// A function returning Suspended (suspended coroutine handle) is an
// initially suspended coroutine that just returns its own handle.
// This is a light-weight means of turning a lambda into a
// coroutine_handle for a suspended coroutine.
using Suspended = std::coroutine_handle<detail::SuspendedCoroPromise>;

} // namespace x23

template<typename... Args>
struct std::coroutine_traits<x23::Suspended, Args...> {
  using promise_type = x23::detail::SuspendedCoroPromise;
};

namespace x23 {

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
    template<typename... Args> requires has_location<Args...>
    promise_type(Args &&...args) : Awaitable<R>(extract_location(args...))
    {}
    promise_type(source_location sloc = source_location::current())
      : Awaitable<R>(sloc)
    {}
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
  CoResume operator()(detail::is_callback_of<R> auto /*cb*/
                          SLOC_DECL) // NOLINT(misc-unused-parameters)
  {
    // To defer destroying callee_, move Async into this coroutine
    Async self = std::move(*this);
    // If you see a segfault on the line below, it may be because you wrote a
    // function that returns an [x23::Async<T>] and you forgot to include a
    // return/co_return statement at the end of the function.
    co_return co_await self;
  }
  template<std::convertible_to<Callback<R>> CB>
  requires (!detail::is_callback_of<CB, R>)
  void operator()(CB &&cb SLOC_DECL)
  {
    (*this)(Callback<R>{std::forward<CB>(cb)} SLOC_COMMA_VAR);
  }

  // Return the Async value synchronously, pausing the current thread
  // until the result is ready.
  R operator()(SLOC_DECL0) // NOLINT(misc-unused-parameters)
  {
    extern bool in_thread_pool();
    x23assert(!in_thread_pool());

    promise_type &p = callee_.promise();
    if (!p.await_ready()) {
      std::binary_semaphore wait(0);
      auto h = [&wait](this auto) -> Suspended {
        wait.release();
        co_return;
      }();
      if (p.await_suspend(h))
        wait.acquire();
      else
        h.destroy();
    }
    return p.await_resume();
  }

private:
  handle_type callee_;
  Async(handle_type h) : callee_(h) { h.promise().launch(); }
};

namespace detail {

// The is_awaiter concept is implemented as a template type to allow
// it to be partially specialized.
// NOLINTBEGIN(readability-identifier-naming)
template<typename A>
    struct is_awaiter_impl : std::conditional_t < requires(A a) {
  {a.await_ready()}->std::same_as<bool>;
  a.await_suspend(std::noop_coroutine());
  a.await_resume();
}, std::true_type, std::false_type>{};
// NOLINTEND(readability-identifier-naming)

// A concept that's 99% likely to determine whether a type is an
// awaiter.  The exception is awaiters that can be awaited only in
// coroutines with certain promise types not including
// std::noop_coroutine_promise.  You can fix any such failing cases by
// partially specializing detail::awaiter_test;
template<typename A> concept is_awaiter = is_awaiter_impl<A>::value;

// Get an awaiter from an awaitable (calling operator co_await if
// there is one).
template<typename T> requires (
    !requires { operator co_await(declval<T>()); } &&
    !requires { declval<T>().operator co_await(); })
inline decltype(auto)
get_awaiter(T &&t) noexcept
{
  return t;
}
template<typename T> requires (
    requires { operator co_await(declval<T>()); } &&
    !requires { declval<T>().operator co_await(); })
inline decltype(auto)
get_awaiter(T &&t) noexcept(noexcept(operator co_await(declval<T>())))
{
  return operator co_await(std::forward<T>(t));
}
template<typename T> requires (requires { declval<T>().operator co_await(); })
inline decltype(auto)
get_awaiter(T &&t) noexcept(noexcept(declval<T>().operator co_await()))
{
  return std::forward<T>(t).operator co_await();
}

// A type you can call co_await on in a generic coroutine.  Does not
// account for the effects of any await_tranform() methods, and does
// not work for specialized awaitables that only work with specific
// promise types.
template<typename A>
concept is_awaitable = is_awaiter<decltype(get_awaiter(declval<A>()))>;

template<is_awaitable A>
using resume_type = decltype(get_awaiter(declval<A>()).await_resume());

template<typename A>
concept resume_noexcept =
    is_awaitable<A> && noexcept(get_awaiter(declval<A>()).await_resume());

} // namespace detail

using detail::is_awaitable;

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
Async<std::decay_t<R>>
async_return(R &&r) noexcept
{
  co_return std::forward<R>(r);
}

// A std::variant with void translated to std::monostate.
template<typename... T>
using void_variant =
    std::variant<std::conditional_t<std::is_void_v<T>, std::monostate, T>...>;

// async_return has a lot of overhead.  Functions that might or might
// not want to allocate a coroutine frame based on runtime conditions
// can alternatively return a MaybeAsync<T>.  It behaves similarly to
// an Async<T> in that you can co_await it or use operator() to get a
// result from non-coroutine code.  However, MaybeAsync doesn't need
// dynamic memory allocation in the event that you produce it with a
// simple return value.
template<typename T, is_awaitable Awaitable = Async<T>>
struct MaybeAsync : void_variant<Awaitable, T> {
  using super = void_variant<Awaitable, T>;
  using value_type = T;
  using awaitable_type = Awaitable;

  using super::super;
  template<std::same_as<T> U = T> requires std::is_void_v<U>
  MaybeAsync() noexcept : super{std::in_place_type<std::monostate>}
  {}

  // Pass through any function call operators on async_type
  template<typename... Args> requires (sizeof...(Args) <= 1)
  decltype(auto) operator()(Args &&...args)
      noexcept(noexcept((*asyncp())(std::declval<Args>()...)))
  {
    if (has_value()) {
      if constexpr (sizeof...(Args) == 0)
        return xval();
      else
        return compose(std::forward<Args>(args)..., &MaybeAsync::xval, this);
    }
    else {
      auto *a = asyncp();
      x23assert(a);
      return (*a)(std::forward<Args>(args)...);
    }
  }

  bool await_ready() noexcept(noexcept(awaiter().await_ready()))
  {
    if (has_value())
      return true;
    return awaiter().await_ready();
  }
  auto await_suspend(auto h) noexcept { return awaiter().await_suspend(h); }
  T await_resume() noexcept(noexcept(awaiter().await_resume()))
  {
    if (has_value())
      return xval();
    return awaiter().await_resume();
  }

  bool has_value() const noexcept { return this->index() == 1; }

private:
  T xval() noexcept
  {
    if constexpr (!std::is_void_v<T>)
      return std::move(get<1>(*this));
    else
      x23assert(has_value());
  }
  awaitable_type *asyncp() noexcept { return get_if<0>(this); }
  decltype(auto) awaiter() noexcept { return detail::get_awaiter(*asyncp()); }
};
template<typename T> MaybeAsync(T) -> MaybeAsync<T>;
template<is_awaitable A> MaybeAsync(A) -> MaybeAsync<detail::resume_type<A>, A>;

} // namespace x23

#if HAVE_BROKEN_CORO_GRO_CLEANUP
// This works around the following bug in clang 21.  The code should
// be deleted once clang21 is obsolete.
// https://github.com/llvm/llvm-project/issues/193412
template<typename T, typename A, typename... Args>
struct std::coroutine_traits<x23::MaybeAsync<T, A>, Args...> {
  using async_promise =
      typename std::coroutine_traits<A, Args...>::promise_type;
  struct promise_type : async_promise {
    using async_promise::async_promise;
    x23::MaybeAsync<T, A> get_return_object()
    {
      return async_promise::get_return_object();
    }
  };
};

#else // HAVE_BROKEN_CORO_GRO_CLEANUP
template<typename T, typename A, typename... Args>
struct std::coroutine_traits<x23::MaybeAsync<T, A>, Args...>
  : std::coroutine_traits<A, Args...> {};

#endif // !HAVE_BROKEN_CORO_GRO_CLEANUP

namespace x23 {

// Generate an Async and matching CoroCB.  This is for use in
// low-level non-coroutine functions that want to return an Async for
// coroutines to co_await.
template<typename R>
std::pair<Async<R>, CoroCB<R>>
make_async(source_location sloc = source_location::current())
{
  CoroCB<R> cb;
  Async<R> async = [&cb](source_location) -> Async<R> {
    // This is not an MVC because GetCoro never suspends.
    auto h = co_await GetCoro<typename Async<R>::promise_type>{};
    cb = h.promise();
    co_await std::suspend_always{};
    std::unreachable();
  }(sloc);
  return std::pair{std::move(async), cb};
}

// A function that returns Detached is a detached coroutine that
// cannot be awaited.  The coroutine destroys itself upon co_return.
//
// initial_suspend returns suspend_always so that the ramp function
// completes (and its parameter copies are destroyed) before the
// coroutine body starts.  The body is started by ~Detached(), which
// runs at the end of the full expression that created the coroutine.
// Without this, the coroutine body starts inside the ramp function,
// and if it suspends (co_await), another thread can resume and
// destroy the coroutine frame while the ramp function is still
// cleaning up parameter copies that alias the frame.
struct Detached {
  // NOLINTBEGIN(readability-convert-member-functions-to-static)
  struct promise_type : PromiseBase {
    template<typename... Args> requires has_location<Args...>
    promise_type(Args &&...args) : PromiseBase(extract_location(args...))
    {}
    promise_type(SLOC_DECL0) : PromiseBase(SLOC_VAR) {}
    Detached get_return_object() noexcept
    {
      return Detached{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept
    {
      trace_unhandled_exception(this);
    }
    std::suspend_never final_suspend() const noexcept { return {}; }
  };
  // NOLINTEND(readability-convert-member-functions-to-static)

  ~Detached()
  {
    if (handle_)
      handle_.resume();
  }
  Detached(Detached &&o) noexcept : handle_(std::exchange(o.handle_, {})) {}

private:
  std::coroutine_handle<promise_type> handle_;
  explicit Detached(std::coroutine_handle<promise_type> h) noexcept : handle_(h)
  {}
};

// Apply a function to the result of co_awaiting an awaitable.  If the
// co_await returns void, then the function takes no arguments.
// Otherwise, the function takes one argument which is the result of
// the co_await.
template<typename F, typename A>
requires is_awaitable<A> && void_invocable<F, detail::resume_type<A>>
struct [[nodiscard]] MapAwait {
  [[no_unique_address]] F func;
  [[no_unique_address]] A awaitable;

  decltype(auto) a() { return detail::get_awaiter(awaitable); }

  bool await_ready() noexcept(noexcept(a().await_ready()))
  {
    return a().await_ready();
  }

  auto await_suspend(auto h SLOC_DECL) noexcept
  {
#if X23_CORO_TRACE
    if constexpr (requires { a().await_suspend(h, sloc); })
      return a().await_suspend(h, sloc);
    else
#endif // X23_CORO_TRACE
      return a().await_suspend(h);
  }

  decltype(auto) await_resume()
      noexcept(noexcept_invocable<F, detail::resume_type<A>> &&
               detail::resume_noexcept<A>)
  {
    return compose(func,
                   [this] -> decltype(auto) { return a().await_resume(); });
  }
};

namespace detail {
// If final_suspend doesn't suspend a coroutine, then the coroutine
// state is implicitly destroyed.  We emulate this behavior.  In the
// event that final_suspend().await_suspend() returns a
// coroutine_handle to jump to, we also return this handle.
template<typename Promise>
auto // either void or coroutine_handle<P>
emulate_final_suspend(std::coroutine_handle<Promise> h)
{
  auto a = get_awaiter(h.promise().final_suspend());
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

  bool await_ready() noexcept { return r_.has_value(); }
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
x23::detail::ExpectAwait<T, E>
operator co_await(std::expected<T, E> &&r)
{
  return x23::detail::ExpectAwait{std::move(r)};
}
template<typename T, typename E>
x23::detail::ExpectAwait<T, E>
operator co_await(const std::expected<T, E> &r)
{
  return x23::detail::ExpectAwait{r};
}
