// -*- C++ -*-

#pragma once

#include <atomic>
#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <source_location>
#include <string>
#include <type_traits>

#include <x23/config.h>
#include <x23/debug.h>
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

namespace detail {

inline void *
allocate_aligned_frame(std::size_t size, std::align_val_t align)
{
  std::size_t alignment = static_cast<std::size_t>(align);
  std::size_t bytes = size + alignment - 1 + sizeof(void *);
  if (bytes < size) [[unlikely]]
    throw std::bad_alloc{};

  void *raw = std::malloc(bytes);
  if (!raw) [[unlikely]]
    throw std::bad_alloc{};

  auto base = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void *);
  auto aligned =
      reinterpret_cast<void *>((base + alignment - 1) & ~(alignment - 1));
  reinterpret_cast<void **>(aligned)[-1] = raw;
  return aligned;
}

inline void
deallocate_aligned_frame(void *ptr) noexcept
{
  if (ptr)
    std::free(reinterpret_cast<void **>(ptr)[-1]);
}

} // namespace detail

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

  // Work around GCC bug #104177: coroutine frames are allocated via
  // default operator new (16-byte aligned on x86-64) but may contain
  // locals with stricter alignment (e.g., alignas(64)).  GCC never
  // calls the aligned overload (size_t, align_val_t) — that IS the
  // bug — so we override the unaligned overload to always return
  // cache-line-aligned memory.  This doesn't help alignas(N) for
  // N > hardware_destructive_interference_size, but nothing in
  // practice needs that.  On ARM the bug is latent (misaligned
  // accesses don't fault) but this avoids the perf penalty too.
  static constexpr std::align_val_t kFrameAlign{
      hardware_destructive_interference_size};
  void *operator new(std::size_t size)
  {
    return detail::allocate_aligned_frame(size, kFrameAlign);
  }
  void operator delete(void *ptr, std::size_t size)
  {
    detail::deallocate_aligned_frame(ptr);
  }
  void operator delete(void *ptr) noexcept
  {
    detail::deallocate_aligned_frame(ptr);
  }

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
    pb_.store(ch.pb_, std::memory_order_relaxed);
#ifdef NDEBUG
    h_.store(ch, std::memory_order_release);
#else  // !NDEBUG
    auto h = h_.exchange(ch, std::memory_order_release);
    x23assert(!h);
#endif // !NDEBUG
  }
  CoroHandle take()
  {
    auto h = h_.exchange(nullptr, std::memory_order_acquire);
    PromiseBase *pb =
        h ? pb_.exchange(nullptr, std::memory_order_relaxed) : nullptr;
    return {h, pb};
  }
  explicit operator bool() const
  {
    return bool{h_.load(std::memory_order_relaxed)};
  }
  PromiseBase *base() const { return pb_; }

private:
  std::atomic<std::coroutine_handle<>> h_;
  std::atomic<PromiseBase *> pb_{nullptr};
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
  // Work around GCC bug #104177 (see trace-enabled PromiseBase above).
  static constexpr std::align_val_t kFrameAlign{
      hardware_destructive_interference_size};
  void *operator new(std::size_t size)
  {
    return detail::allocate_aligned_frame(size, kFrameAlign);
  }
  void operator delete(void *ptr, std::size_t size)
  {
    detail::deallocate_aligned_frame(ptr);
  }
  void operator delete(void *ptr) noexcept
  {
    detail::deallocate_aligned_frame(ptr);
  }

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

// Trace the chain of coroutines leading to a particular promise object.
std::string trace(const PromiseBase *pb, unsigned indent = 0,
                  std::string ret = {});

// If you compiled with X23_CORO_TRACE and are in a coroutine whose
// base type is derived from PromiseBase, you can retrieve a trace of
// the current coroutine with:
//
//   std::string ct = co_await GetCoroTrace{};
struct [[nodiscard]] GetCoroTrace {
  const PromiseBase *pb_{};
  constexpr bool await_ready() const noexcept { return false; }
  template<typename P> bool await_suspend(std::coroutine_handle<P> h)
  {
    if constexpr (requires { pb_ = &h.promise(); })
      pb_ = std::addressof(h.promise());
    return false;
  }
  std::string await_resume() const { return pb_ ? trace(pb_) : std::string{}; }
};

// Dump trace information about an unhandled exception before
// terminating.
[[noreturn]] void trace_unhandled_exception(const PromiseBase *pb) noexcept;

// True if an argument list ends with a source_location following at
// least one other argument.
template<typename... Args>
concept has_location =
    (sizeof...(Args) > 1) && decays_to<LastType<Args...>, source_location>;

template<typename... Args> requires has_location<Args...>
inline source_location
extract_location(const Args &...args)
{
  return detail::last_arg(args...);
}

} // namespace x23
