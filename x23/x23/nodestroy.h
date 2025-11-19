// -*- C++ -*-

#pragma once

#include <atomic>

#include <x23/utility.h>

#if __has_include(<sanitizer/lsan_interface.h>)
#include <sanitizer/lsan_interface.h>
#endif // !__has_include(<sanitizer/lsan_interface.h>)

#if !defined(__SANITIZE_ADDRESS__) &&                                          \
    (!defined(__has_feature) || !__has_feature(address_sanitizer))
#define __lsan_ignore_object(ptr) ((void)0)
#endif // address sanitizer disabled

namespace x23 {

// When creating global objects that should not be destroyed at
// shutdown, wrap them in make_nodestroy to avoid address sanitizer
// warnings about leaks.  For example:
//
// auto &my_global_map = make_nodestroy(new std::map<int, std::string>);
inline decltype(auto)
make_nodestroy(auto *p)
{
  __lsan_ignore_object(p);
  return *p;
}

// A global object of type NoDestroy<T> contains a T that will not be
// destroyed at global destruction.  If ManualConstruct is true, the
// object will not be constructed at global initialization, either,
// and you will have to construct it manually by calling the construct
// method.
template<typename T, bool ManualConstruct = false> union NoDestroy {
  T value;

  template<bool Valid = !ManualConstruct, typename... Args> requires Valid
  NoDestroy(Args &&...args) : value(std::forward<Args>(args)...)
  {}

  template<bool Valid = ManualConstruct> requires Valid
  NoDestroy() noexcept
  {}

  NoDestroy(const NoDestroy &) = delete;
  ~NoDestroy() {}

  template<bool Valid = ManualConstruct, typename... Args>
  requires Valid && (Valid == ManualConstruct)
  decltype(auto) construct(Args &&...args)
  {
    return *std::construct_at(get(), std::forward<Args>(args)...);
  }

  auto get(this auto &&self) noexcept { return &self.value; }
  decltype(auto) operator*(this auto &&self) noexcept { return (self.value); }
  auto operator->(this auto &&self) noexcept { return &self.value; }
};
template<typename T> NoDestroy(T &&) -> NoDestroy<T, false>;

} // namespace x23
