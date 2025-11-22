// -*- C++ -*-

#pragma once

#include <atomic>
#include <initializer_list>
#include <type_traits>

#include <x23/utility.h>

#if __has_include(<sanitizer/lsan_interface.h>)
#include <sanitizer/lsan_interface.h>
#endif // !__has_include(<sanitizer/lsan_interface.h>)

#if !defined(__SANITIZE_ADDRESS__) &&                                          \
    (!defined(__has_feature) || !__has_feature(address_sanitizer))
#define __lsan_ignore_object(ptr) ((void)0)
struct [[maybe_unused]] LsanDisabler {};
#else  // address sanitizer
using LsanDisabler = __lsan::ScopedDisabler;
#endif // address sanitizer

namespace x23 {

// Mark a pointer as okay to leak (if address sanitizer is enabled),
// and return a reference to what it points to.  For example, you
// might want to do this to create a leaky map, although see NoDestroy
// below for a better way:
//
// auto &my_global_map = make_nodestroy(new std::map<int, std::string>);
inline decltype(auto)
make_nodestroy(auto *p)
{
  __lsan_ignore_object(p);
  return *p;
}

// This is about 10x faster than std::call_once with gcc, and exactly
// the same cost as using an already initialized static.  With clang,
// it's slightly slower than using a static but still the same as or
// faster than std::call_once.
class Once {
public:
  // Perform an action the first time it is called, immediately return
  // on subsequent invocations.
  void operator()(auto &&f) noexcept
      requires requires { std::forward<decltype(f)>(f)(); }
  {
    auto stage = flag_.load(std::memory_order_acquire);
    if (stage > 0) [[likely]]
      return;
    if (stage == 0 && flag_.compare_exchange_strong(stage, -1)) {
      // Calling f() directly benchmarks a hair faster than std::invoke
      std::forward<decltype(f)>(f)();
      flag_.store(1, std::memory_order_release);
      flag_.notify_all();
    }
    else
      flag_.wait(-1);
  }

  bool used() const noexcept
  {
    return flag_.load(std::memory_order_relaxed) > 0;
  }

private:
  //  0 - unconstructed
  // -1 - construction in progress
  //  1 - constructed
  //
  // [On x86, you can get the sign with "test %al,%al", which encodes
  // in 2 bytes, one fewer than checking against an immediate constant
  // like $1 or $2.]
  std::atomic_int8_t flag_;
};
// Ensure constexpr initialization and no non-trivial destruction
static_assert((void(Once{}), std::is_trivially_destructible_v<Once>));

// NoDestroy<T> embeds an element of class T, a bit like optional<T>,
// except the object is never destroyed.  Hence, it's useful for
// globals and statics that might be accessed during global
// destruction.
template<typename T> class NoDestroy {
public:
  using value_type = T;

  template<class... Args> requires std::constructible_from<T, Args...>
  constexpr NoDestroy(Args &&...args)
      noexcept(noexcept(T(std::forward<Args>(args)...)))
  {
    LsanDisabler _inhibit;
    std::construct_at(&val_, std::forward<Args>(args)...);
  }

  template<class U, class... Args>
  requires std::constructible_from<T, std::initializer_list<U>, Args...>
  constexpr NoDestroy(std::initializer_list<U> ilist, Args &&...args)
      noexcept(noexcept(T(ilist, std::forward<Args>(args)...)))
  {
    LsanDisabler _inhibit;
    std::construct_at(&val_, ilist, std::forward<Args>(args)...);
  }

  constexpr ~NoDestroy() {}

  auto *get(this auto &&self) noexcept { return &self.val_; }
  auto &operator*(this auto &&self) noexcept { return (self.val_); }
  auto *operator->(this auto &&self) noexcept { return &self.val_; }

private:
  union {
    std::remove_const_t<value_type> val_;
  };
};
template<typename T> NoDestroy(T &&) -> NoDestroy<std::remove_cvref_t<T>>;

} // namespace x23
