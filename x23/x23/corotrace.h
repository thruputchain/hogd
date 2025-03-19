// -*- C++ -*-

#pragma once

#ifndef X23_CORO_TRACE
#ifdef NDEBUG
#define X23_CORO_TRACE 0
#else // !NDEBUG
#define X23_CORO_TRACE 1
#endif // !NDEBUG
#endif // !defined(X23_CORO_TRACE)

#include <cstring>
#include <format>
#include <source_location>
#if HAVE_STD_STACKTRACE
#include <stacktrace>
#endif // HAVE_STD_STACKTRACE

namespace x23 {

using std::source_location;

inline std::string
to_string(const source_location &s)
{
  return std::format("{}:{}", s.file_name(), s.line());
}

#if HAVE_STD_STACKTRACE

inline std::string
get_callsite(std::string_view ignore_file,
	     source_location fallback = source_location::current())
{
  static const std::string_view this_file(__FILE__);

  auto bt = std::stacktrace::current();
  for (const auto &e : bt)
    if (e.source_line())
      if (std::string sf = e.source_file();
	  sf != this_file &&
	  (ignore_file.empty() || !sf.ends_with(ignore_file)))
	return std::format("*{}:{}", sf, e.source_line());
  return to_string(fallback);
}

#else // !HAVE_STD_STACKTRACE

inline std::string
get_callsite(std::string_view ignore_file,
	     source_location sloc = source_location::current())
{
  return to_string(sloc);
}

#endif // !HAVE_STD_STACKTRACE

} // namespace x23

#if X23_CORO_TRACE

#define X23_IFTRACE(...) __VA_ARGS__

#include <x23/config.h>

#include <coroutine>
#include <print>
#include <unordered_map>

#include <x23/mvar.h>
#include <x23/utility.h>
#include <x23/vec.h>

#define SLOC_ARG0 std::source_location sloc
#define SLOC_ARG , SLOC_ARG0
#define SLOC_PARM0 std::source_location sloc = std::source_location::current()
#define SLOC_PARM , SLOC_PARM0
#define SLOC_VAR sloc

namespace x23 {

// Extract a source_location from the end of a list of arguments if
// one is there, otherwise return the source_location where the GetLoc
// was allocated.  Usage:
//
//    struct my_promise_type {
//      my_promise_type(const auto &...args) {
//        corotrace.set_alloc(h, GetLoc{}(args...));
//      }
//      ...
//    };
struct GetLoc {
  source_location sloc_;
  bool from_args_{};

  GetLoc(source_location sloc = source_location::current()) noexcept
    : sloc_(sloc)
  {}
  GetLoc &operator()(const auto &...args)
  {
    using detail::last_arg;
    if constexpr (requires {
                    { last_arg(args...) } -> references<source_location>;
                  }) {
      sloc_ = last_arg(args...);
      from_args_ = true;
    }
    return *this;
  }

  std::string description() const
  {
    using namespace std::string_view_literals;
    if (!from_args_)
      return get_callsite("/x23/async.h"sv, sloc_);
    return to_string(sloc_);
  }
};

inline std::string
to_string(const GetLoc &gl)
{
  return gl.description();
}

struct CoroTrace {
  struct TraceState {
    Str alloc_point;
    Str suspend_point;
  };

  int enable_{};
  MVar<std::unordered_map<std::coroutine_handle<>, TraceState>> traces_;

  static Str locformat(std::coroutine_handle<> h, const auto &sloc,
                       const auto &...extra)
  {
    Str ret = uformat("{}: [{}]", to_string(sloc), h.address());
    if constexpr (sizeof...(extra)) {
      auto append = [&ret](const auto &a) { ret += uformat(" {}", a); };
      (append(extra), ...);
    }
    return ret;
  }

  void set_suspend(std::coroutine_handle<> h, const source_location &sloc,
                   const auto &...extra)
  {
    if (enable_) {
      Str msg = locformat(h, sloc, extra...);
      if (enable_ > 1)
        std::println("SUSPEND: {}", msg);
      auto t = traces_.take();
      auto it = t->try_emplace(h).first;
      it->second.suspend_point = std::move(msg);
    }
  }
  void clear_suspend(std::coroutine_handle<> h)
  {
    if (enable_) {
      auto t = traces_.take();
      auto it = t->find(h);
      if (it != t->end()) {
        if (enable_ > 1)
          std::println("RESUME: {}", it->second.suspend_point);
        if (it->second.alloc_point.empty())
          t->erase(it);
        else
          it->second.suspend_point.clear();
      }
    }
  }

  void set_alloc(std::coroutine_handle<> h, const GetLoc &sloc,
                 const auto &...extra)
  {
    if (enable_) {
      Str msg = locformat(h, sloc, extra...);
      if (enable_ > 1)
        std::println("ALLOC: {}", msg);
      traces_->insert_or_assign(h, TraceState{.alloc_point = std::move(msg)});
    }
  }
  void clear_alloc(std::coroutine_handle<> h)
  {
    if (enable_) {
      if (enable_ > 1)
        std::println("FREE: [{}]", h.address());
      traces_->erase(h);
    }
  }
};

inline CoroTrace corotrace;

} // namespace x23

#else // !X23_CORO_TRACE

#define X23_IFTRACE(...)

#define SLOC_PARM0
#define SLOC_PARM
#define SLOC_ARG0
#define SLOC_ARG
#define SLOC_VAR

#endif // !X23_CORO_TRACE
