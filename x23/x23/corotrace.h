// -*- C++ -*-

#pragma once

#include <x23/config.h>
#include <x23/utility.h>

#ifndef X23_CORO_TRACE
#if defined(NDEBUG) || !X23_CORO_SLOC_OK
#define X23_CORO_TRACE 0
#else // !NDEBUG && X23_CORO_TRACE
#define X23_CORO_TRACE 1
#endif // !NDEBUG && X23_CORO_TRACE
#endif // !defined(X23_CORO_TRACE)

#include <concepts>
#include <coroutine>
#include <source_location>
#include <type_traits>

namespace x23 {

using std::source_location;

#if X23_CORO_TRACE

#define X23_IFTRACE(...) __VA_ARGS__

// A parameter of the current source location for a function declaration
#define SLOC_DECL0 std::source_location sloc = std::source_location::current()

// Append an argument taking source location to function declaration
#define SLOC_DECL , SLOC_DECL0

// Like SLOC_DECL/SLOC_DECL0 for definitions of previously declared functions
#define SLOC_ARG0 std::source_location sloc
#define SLOC_ARG , SLOC_ARG0

// The variable containing a source location, if declared by above macros
#define SLOC_VAR sloc
#define SLOC_COMMA_VAR , SLOC_VAR

struct PromiseBase {
  source_location creation_{};
  source_location suspension_{};
  PromiseBase *waiter_{}; // non-NULL if another coro is co_awaiting us

  PromiseBase(source_location sloc) noexcept : creation_(sloc) {}

  template<typename P> requires (!std::is_void_v<P>)
  static PromiseBase *get(std::coroutine_handle<P> h) noexcept
  {
    if constexpr (std::derived_from<P, PromiseBase>)
      return std::addressof(h.promise());
    return nullptr;
  }
};

struct CoroHandle : std::coroutine_handle<> {
  PromiseBase *pb_;

  template<typename P> requires (!std::is_void_v<P>)
  CoroHandle(std::coroutine_handle<P> h) noexcept
    : std::coroutine_handle<>(h), pb_(PromiseBase::get(h))
  {}
  PromiseBase *base() const { return pb_; }
};

#else // !X23_CORO_TRACE

#define X23_IFTRACE(...)

#define SLOC_DECL0
#define SLOC_DECL
#define SLOC_ARG0
#define SLOC_ARG
#define SLOC_VAR
#define SLOC_COMMA_VAR

struct PromiseBase {
  PromiseBase() noexcept = default;
  PromiseBase(source_location) noexcept {}

  template<typename P> requires (!std::is_void_v<P>)
  static PromiseBase *get(std::coroutine_handle<P>) noexcept
  {
    return nullptr;
  }
};

using CoroHandle = std::coroutine_handle<>;

#endif // !X23_CORO_TRACE

} // namespace x23
