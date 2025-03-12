// -*- C++ -*-

#pragma once

#ifndef X23_CORO_TRACE
#ifdef NDEBUG
#define X23_CORO_TRACE 0
#else // !NDEBUG
#define X23_CORO_TRACE 1
#endif // !NDEBUG
#endif // !defined(X23_CORO_TRACE)

#include <typeinfo>

#if HAVE_CXA_DEMANGLE

#include <cxxabi.h>
#include <string>
#include <string_view>

namespace x23 {
template<typename T>
const std::string_view
type_name()
{
  static const std::string ret = [] -> std::string {
    const char *mangled = typeid(T).name();
    int status;
    char *demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    if (status)
      return mangled;
    std::string ret(demangled);
    free(demangled);
    return ret;
  }();
  return ret;
}
} // namespace x23

#else // !HAVE_CXA_DEMANGLE

namespace x23 {
template<typename T>
const char *
type_name()
{
  return typeid(T).name();
}
}

#endif // !HAVE_CXA_DEMANGLE

#if X23_CORO_TRACE

#include <coroutine>
#include <print>
#include <source_location>
#include <unordered_map>

#include <x23/mvar.h>
#include <x23/utility.h>
#include <x23/vec.h>

#define SLOC_ARG0 std::source_location sloc
#define SLOC_ARG , SLOC_ARG0
#define SLOC_PARM0 std::source_location sloc = std::source_location::current()
#define SLOC_PARM , SLOC_PARM0

namespace x23 {

using std::source_location;

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
  source_location fallback_;
  GetLoc(source_location fb = source_location::current()) noexcept
    : fallback_(fb)
  {}
  source_location operator()(const auto &...args) const noexcept
  {
    using detail::last_arg;
    if constexpr (requires {
                    { last_arg(args...) } -> references<source_location>;
                  })
      return last_arg(args...);
    else
      return fallback_;
  }
};

struct CoroTrace {
  struct TraceState {
    Str alloc_point;
    Str suspend_point;
  };

  int enable_{};
  MVar<std::unordered_map<std::coroutine_handle<>, TraceState>> traces_;

  static Str locformat(std::coroutine_handle<> h, source_location sloc,
                       const auto &...extra)
  {
    Str ret =
        uformat("{}:{}: [{}]", sloc.file_name(), sloc.line(), h.address());
    if constexpr (sizeof...(extra)) {
      auto append = [&ret](const auto &a) { ret += uformat(" {}", a); };
      (append(extra), ...);
    }
    return ret;
  }

  void set_suspend(std::coroutine_handle<> h, source_location sloc,
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

  void set_alloc(std::coroutine_handle<> h, source_location sloc,
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

extern CoroTrace corotrace;

#else // !X23_CORO_TRACE

#define SLOC_PARM0
#define SLOC_PARM
#define SLOC_ARG0
#define SLOC_ARG

#endif // !X23_CORO_TRACE

} // namespace x23
