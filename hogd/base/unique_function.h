// unique_function.h - provides a move-only polymorphic function wrapper
//
// This is a temporary measure until std::move_only_function is implemented in
// libc++ (Clang). This version is reasonably fast, but is missing the no heap
// allocation optimization for small invocables.

#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <version>

#if defined(__cpp_lib_move_only)
#include <functional>
#endif // defined(__cpp_lib_move_only)

namespace hogd {

#if defined(__cpp_lib_move_only)

using UniqueFunction = std::move_only_function;

#else // defined(__cpp_lib_move_only)

namespace detail {

template<typename Ret, typename... Args> class InvokerBase {
public:
  virtual ~InvokerBase() = default;
  // NOLINTNEXTLINE(portability-template-virtual-member-function)
  virtual Ret operator()(Args &&...args) = 0;
};

template<typename F, typename Ret, typename... Args>
class Invoker : public InvokerBase<Ret, Args...> {
public:
  Invoker(F &&func) : func_(std::move(func)) {}
  ~Invoker() override = default;

  Ret operator()(Args &&...args) override
  {
    return func_(std::forward<Args>(args)...);
  }

private:
  F func_;
};

} // namespace detail

// invocable_r is the concept for a callable type F that can be called with
// arguments Arg... and returns a type convertable to Ret.
template<typename Ret, typename F, typename... Args>
concept invocable_r = std::is_invocable_r_v<Ret, F, Args...>;

template<typename... Sig> class UniqueFunction {};

// UniqueFunction is a move-only polymorphic function wrapper that provides
// similar functionality to std::move_only_function.
//
// It's main benefit is that it can support non-copyable function objects.
//
// Example:
// UniqueFunction<void()> fn = [ref = std::make_unique<int>(0)]{};
//
// This works with UniqueFunction but not with std::function because ref is
// non-copyable.
template<typename Ret, typename... Args> class UniqueFunction<Ret(Args...)> {
public:
  UniqueFunction() = default;
  template<typename F> requires invocable_r<Ret, F, Args...>
  UniqueFunction(F &&func)
  {
    using Invoker = detail::Invoker<std::decay_t<F>, Ret, Args...>;
    invoker_ = std::make_unique<Invoker>(std::forward<F>(func));
  }
  ~UniqueFunction() = default;

  // Enable move and disable copy.
  UniqueFunction(UniqueFunction &&uf) noexcept
    : invoker_(std::move(uf.invoker_))
  {}
  UniqueFunction &operator=(UniqueFunction &&uf) noexcept
  {
    invoker_ = std::move(uf.invoker_);
    return *this;
  }
  UniqueFunction &operator=(std::nullptr_t) noexcept
  {
    invoker_.reset();
    return *this;
  }
  UniqueFunction(const UniqueFunction &uf) = delete;
  UniqueFunction &operator=(const UniqueFunction &uf) = delete;

  void swap(UniqueFunction &other) noexcept { invoker_.swap(other.invoker_); }

  explicit operator bool() const noexcept { return !!invoker_; }

  Ret operator()(Args &&...args)
  {
    return invoker_->operator()(std::forward<Args>(args)...);
  }

private:
  std::unique_ptr<detail::InvokerBase<Ret, Args...>> invoker_;
};

template<typename Ret, typename... Args>
bool
operator==(const UniqueFunction<Ret, Args...> &f, std::nullptr_t)
{
  return !!f;
}

#endif // defined(__cpp_lib_move_only)

} // namespace hogd
