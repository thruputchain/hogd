// -*-C++-*-

// Clang doesn't yet support std::move_only_function, so implement it
// where required.  x23::move_only_function will be the same as
// std::move_only_function when the latter is implemented.

#pragma once

#include <functional>

namespace x23 {

#if __cpp_lib_move_only_function >= 202110L

using std::move_only_function;

#else // std::move_only_function unimplemented

#include <memory>

template<typename F> struct move_only_function;

template<typename R, typename... Args, bool NE>
class move_only_function<R(Args...) noexcept(NE)> {
public:
  struct Invoker {
    virtual ~Invoker() = default;
    virtual R operator()(Args... args) noexcept(NE) = 0;
  };
  template<typename F> struct Impl : Invoker {
    F f_;
    // Impl(F f) : f_(std::move(f)) {}
    template<typename RF> Impl(RF &&f) : f_(std::forward<RF>(f)) {}
    R operator()(Args... args) noexcept(NE) override
    {
      return std::invoke(f_, std::forward<Args>(args)...);
    }
  };
  template<typename F> Impl(F &&) -> Impl<std::decay_t<F>>;

  std::unique_ptr<Invoker> inv_;

public:
  move_only_function() noexcept = default;
  move_only_function(std::nullptr_t) noexcept {};
  move_only_function(move_only_function &&) noexcept = default;

  template<std::invocable<Args...> F>
  move_only_function(F f) : inv_(new Impl{std::move(f)})
  {}

  move_only_function &operator=(move_only_function &&) noexcept = default;

  R operator()(Args... args) noexcept(NE)
  {
    return (*inv_)(std::forward<Args>(args)...);
  }

  explicit operator bool() noexcept { return inv_; }
  bool operator==(std::nullptr_t) noexcept { return !inv_; }
};

#endif // std::move_only_function unimplemented

} // namespace x23
