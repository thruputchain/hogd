// -*- C++ -*-

#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <coroutine>
#include <source_location>
#include <string>
#include <type_traits>

#include <x23/config.h>
#include <x23/utility.h>

#ifndef X23_CORO_TRACE
#if defined(NDEBUG) || !X23_CORO_SLOC_OK
#define X23_CORO_TRACE 0
#else // !NDEBUG && X23_CORO_TRACE
#define X23_CORO_TRACE 1
#endif // !NDEBUG && X23_CORO_TRACE
#endif // !defined(X23_CORO_TRACE)

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

  PromiseBase(source_location sloc) noexcept
    : creation_(sloc), suspension_(creation_)
  {}

  PromiseBase *waiter() const noexcept { return waiter_; }

  template<typename P> requires (!std::is_void_v<P>)
  static PromiseBase *get(std::coroutine_handle<P> h) noexcept
  {
    if constexpr (std::derived_from<P, PromiseBase>)
      return std::addressof(h.promise());
    return nullptr;
  }
};

struct CoroHandle : std::coroutine_handle<> {
  using super = std::coroutine_handle<>;
  PromiseBase *pb_{};

  CoroHandle() noexcept = default;
  template<typename P> requires (!std::is_void_v<P>)
  CoroHandle(std::coroutine_handle<P> h) noexcept
    : std::coroutine_handle<>(h), pb_(PromiseBase::get(h))
  {}
  CoroHandle(std::coroutine_handle<> h, PromiseBase *pb) : super(h), pb_(pb) {}
  PromiseBase *base() const { return pb_; }
};

class AtomicCoroHandle {
public:
  void put(const CoroHandle &ch)
  {
    pb_ = ch.pb_;
#ifdef NDEBUG
    h_.store(ch, std::memory_order_release);
#else  // !NDEBUG
    auto h = h_.exchange(ch, std::memory_order_release);
    assert(!h);
#endif // !NDEBUG
  }
  CoroHandle take()
  {
    PromiseBase *pb = std::exchange(pb_, nullptr);
    auto h = h_.exchange(nullptr, std::memory_order_acquire);
    return {h, pb};
  }
  explicit operator bool() const
  {
    return bool{h_.load(std::memory_order_relaxed)};
  }
  PromiseBase *base() const { return pb_; }

private:
  std::atomic<std::coroutine_handle<>> h_;
  PromiseBase *pb_{};
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
  PromiseBase *waiter() const noexcept { return nullptr; }
  template<typename P> requires (!std::is_void_v<P>)
  static PromiseBase *get(std::coroutine_handle<P>) noexcept
  {
    return nullptr;
  }
  std::string show() const { return {}; }
};

struct CoroHandle : std::coroutine_handle<> {
  using super = std::coroutine_handle<>;

  CoroHandle() noexcept = default;
  template<typename P>
  CoroHandle(std::coroutine_handle<P> h, PromiseBase * = nullptr) : super(h)
  {}
};

class AtomicCoroHandle {
public:
  void put(const CoroHandle &ch)
  {
#ifdef NDEBUG
    h_.store(ch, std::memory_order_relaxed);
#else  // !NDEBUG
    auto h = h_.exchange(ch, std::memory_order_relaxed);
    assert(!h);
#endif // !NDEBUG
  }
  CoroHandle take()
  {
    return {h_.exchange(nullptr, std::memory_order_relaxed), nullptr};
  }
  explicit operator bool() const
  {
    return bool{h_.load(std::memory_order_relaxed)};
  }
  PromiseBase *base() const { return nullptr; }

private:
  std::atomic<std::coroutine_handle<>> h_;
};

#endif // !X23_CORO_TRACE

std::string trace(const PromiseBase *pb, unsigned indent = 0,
                  std::string ret = {});

} // namespace x23
