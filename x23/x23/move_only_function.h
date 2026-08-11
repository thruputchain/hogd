// -*-C++-*-

#pragma once

// Clang doesn't yet support std::move_only_function, so implement it
// where required.  x23::move_only_function will be the same as
// std::move_only_function when the latter is implemented.

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace x23 {

#if __cpp_lib_move_only_function >= 202110L

using std::move_only_function;

#else // std::move_only_function unimplemented

template<typename F> class move_only_function;

template<typename R, typename... Args, bool NE>
class move_only_function<R(Args...) noexcept(NE)> {
  struct Invoker {
    virtual ~Invoker() = default;
    virtual R operator()(Args... args) noexcept(NE) = 0;
  };
  template<typename F> struct Impl : Invoker {
    F f_;
    explicit Impl(const F &f) : f_(f) {}
    explicit Impl(F &&f) : f_(std::move(f)) {}
    R operator()(Args... args) noexcept(NE) override
    {
      return std::invoke(f_, std::forward<Args>(args)...);
    }
  };

  std::unique_ptr<Invoker> inv_;

public:
  move_only_function() noexcept = default;
  move_only_function(std::nullptr_t) noexcept {}
  move_only_function(move_only_function &&) noexcept = default;

  template<typename F>
  requires (!std::same_as<std::remove_cvref_t<F>, move_only_function> &&
            std::is_constructible_v<std::decay_t<F>, F> &&
            std::is_invocable_r_v<R, std::decay_t<F> &, Args...> &&
            (!NE ||
             std::is_nothrow_invocable_r_v<R, std::decay_t<F> &, Args...>))
  move_only_function(F &&f)
    : inv_(std::make_unique<Impl<std::decay_t<F>>>(std::forward<F>(f)))
  {}

  move_only_function &operator=(move_only_function &&) noexcept = default;

  R operator()(Args... args) noexcept(NE)
  {
    return (*inv_)(std::forward<Args>(args)...);
  }

  explicit operator bool() const noexcept { return bool{inv_}; }
  bool operator==(std::nullptr_t) const noexcept { return !inv_; }
};

#endif // std::move_only_function unimplemented

} // namespace x23
