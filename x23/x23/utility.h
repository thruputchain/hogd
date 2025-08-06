// -*- C++ -*-

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

#if HAVE_BUILTIN_ARM_YIELD
#include <arm_acle.h>
#endif // HAVE_BUILTIN_ARM_YIELD

namespace x23 {

using std::size_t;
using std::uintptr_t;
using std::intptr_t;
using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::int8_t;
using std::int16_t;
using std::int32_t;
using std::int64_t;

// FORWARD(arg) is just std::forward<decltype(arg)>(arg), which
// creates an expression whose type corresponds to the type with which
// variable arg was declared--rvalue reference if arg is an rvalue
// reference, lvalue reference otherwise.
//
// arg should always be an unparenthesized id-expression--usually a
// function parameter declared as a forwarding reference (auto &&arg).
//
// We play some games to force a compliation error in the event that
// arg is parenthesized, as this would cause us to get the wrong
// decltype--i.e., the type of an expression, not the type with which
// a variable was declared.  (An expression is already its own type,
// so you don't need std::forward in that case.)
#define __FORWARD_IGNORE(x) // causes error if invoked with 2 arguments
#define __FORWARD_APPLY_IGNORE(x) __FORWARD_IGNORE(x)
#define __FORWARD_PARENTHESIZED_TO_COMMA(x) ,
#define FORWARD(arg)                                                           \
  ::std::forward<decltype(arg)>(arg)                                           \
      __FORWARD_APPLY_IGNORE(__FORWARD_PARENTHESIZED_TO_COMMA arg)

// Types T and U differ only in reference and const/volatile
// qualifiers or pointer/array.
template<typename T, typename U>
concept decays_to = std::same_as<std::decay_t<T>, std::decay_t<U>>;

// Match an lvalue- or rvalue-, const or non-const reference to T.
template<typename T, typename U>
concept references = std::convertible_to<T, const U &>;

// Type Derived is a posibly cv- or reference-qualified Base or
// subtype of Base.
template<typename Derived, typename Base>
concept derived_ref = std::derived_from<std::remove_cvref_t<Derived>, Base>;

template<typename T, typename... Ts>
concept is_one_of = (std::same_as<T, Ts> || ...);

template<typename RG, typename T>
concept is_range_of =
    std::ranges::range<RG> && std::same_as<std::ranges::range_value_t<RG>, T>;

template<typename RG, typename T>
concept is_forward_range_of =
    is_range_of<RG, T> && std::ranges::forward_range<RG>;

// Lambda with ad-hoc polymorphism.  For example:
//
//   auto f = Multilambda{
//     [](int i) { /* ... do int thing ... */ },
//     [](std::string_view sv) { /* ... do string thing ... */ },
//   };
//
// See here for an explanation of how this works:
// https://www.scs.stanford.edu/~dm/blog/param-pack.html#multilambda
template<typename... Lambdas> struct Multilambda : Lambdas... {
  using Lambdas::operator()...;
};
template<typename... Lambdas>
Multilambda(Lambdas...) -> Multilambda<Lambdas...>;

// true if T is the type TMPL<Args...> for some Args...  However,
// doesn't work if TMPL has any non-type template arguments.
template<typename T, template<typename...> typename Tmpl>
concept is_template_instance = decltype(Multilambda{
    []<typename... U>(const Tmpl<U...> &) { return std::true_type{}; },
    [](const auto &) { return std::false_type{}; },
}(std::declval<T>()))::value;

namespace detail {
template<size_t N, typename T0, typename... T> requires (N <= sizeof...(T))
struct TypeNumHelper {
  using type = typename TypeNumHelper<N - 1, T...>::type;
};
template<typename T0, typename... T> struct TypeNumHelper<0, T0, T...> {
  using type = T0;
};

inline decltype(auto)
last_arg(auto &&arg0, auto &&...rest)
{
  if constexpr (sizeof...(rest) == 0)
    return arg0;
  else
    return last_arg(std::forward<decltype(rest)>(rest)...);
}
} // namespace detail

// type_num<N, T0, T1, ...> is just TN
template<size_t N, typename... T> requires (N < sizeof...(T))
using TypeNum = detail::TypeNumHelper<N, T...>::type;

// type_num<T0, T1, ..., Tmax> is just Tmax
template<typename... T> requires (sizeof...(T) > 0)
using LastType = TypeNum<sizeof...(T) - 1, T...>;

// Make it easy to check inclusion of multiple categories represented
// as an enum class, e.g.:
//
//   if (c & FlagSet{kVarBytes, kFixedBytes}) ...
template<typename E> requires std::is_enum_v<E>
struct FlagSet {
  using enum_type = E;
  using set_type = std::underlying_type_t<E>;

  set_type flags_{};

  constexpr FlagSet() noexcept = default;
  constexpr FlagSet(enum_type e) noexcept : flags_(set_type(e)) {}
  constexpr FlagSet(std::initializer_list<enum_type> il) noexcept
  {
    for (auto e : il)
      flags_ |= set_type(e);
  }
  constexpr explicit operator bool() const noexcept { return flags_; }
  constexpr FlagSet operator&(FlagSet e) const noexcept
  {
    FlagSet ret;
    ret.flags_ = flags_ & e.flags_;
    return ret;
  }
  constexpr friend FlagSet operator&(enum_type e, FlagSet s) noexcept
  {
    return FlagSet{e} & s;
  }
};

enum class CallableCategory {
  kFunction = 1 << 1,
  kMethod = 1 << 2,
  kObject = 1 << 3
};

// Function traits, allow you to extract argument and return types
// from a function.
template<typename F> struct FnTraits;
template<typename R, typename... A, bool NE> requires (sizeof...(A) > 0)
struct FnTraits<R (*)(A...) noexcept(NE)> {
  using return_type = R;
  static constexpr size_t num_args = sizeof...(A);
  template<size_t N> requires (N < num_args)
  using arg_type = TypeNum<N, A...>;
  static constexpr bool is_noexcept = NE;
  static constexpr CallableCategory category = CallableCategory::kFunction;
};
template<typename R, bool NE> struct FnTraits<R (*)() noexcept(NE)> {
  using return_type = R;
  static constexpr size_t num_args = 0;
  static constexpr bool is_noexcept = NE;
  static constexpr CallableCategory category = CallableCategory::kFunction;
};
template<typename R, typename S, typename... A, bool NE>
struct FnTraits<R (S::*)(A...) noexcept(NE)>
  : FnTraits<R (*)(A...) noexcept(NE)> {
  using object_type = S;
  static constexpr CallableCategory category = CallableCategory::kMethod;
};
template<typename R, typename S, typename... A, bool NE>
struct FnTraits<R (S::*)(A...) const noexcept(NE)>
  : FnTraits<R (*)(A...) noexcept(NE)> {
  using object_type = S;
  static constexpr CallableCategory category = CallableCategory::kMethod;
};
template<typename Obj> requires requires { &Obj::operator(); }
struct FnTraits<Obj> : FnTraits<decltype(&Obj::operator())> {
  static constexpr CallableCategory category = CallableCategory::kObject;
};
template<typename F> requires (!std::same_as<F, std::decay_t<F>>)
struct FnTraits<F> : FnTraits<std::decay_t<F>> {};

template<size_t N, typename F>
using ArgNumType = typename FnTraits<F>::template arg_type<N>;

// variant_index<std::variant<T0, T1, T2>, Tn> is 0 when Tn is T0, 1
// when Tn is T1, etc.
template<typename U, typename T>
constexpr size_t variant_index =
    []<template<typename...> typename V, typename... Ts>(V<Ts...> *) {
      size_t ret = 0;
      if (((std::same_as<Ts, T> || (++ret, false)) || ...))
        return ret;
      // Cause a constexpr compilation error if type not in argument:
      throw std::logic_error("type not in argument list");
    }(static_cast<U *>(nullptr));

// Try to find a common return type for a bunch of functions, but fall
// back to void if any of the types are void or they otherwise don't
// have a common type.
template<typename... T>
using merged_return_t = decltype([] {
  if constexpr (requires { typename std::common_type_t<T...>; })
    return [] -> std::common_type_t<T...> { throw; }();
  else
    return;
}());

// Tag type to request that the return type of with_n be inferred.
struct auto_t {};

// Convert a run-time size_t n into a compile-time
// std::integral_constant, and invoke a function object on that
// constant.  You must supply a Max value of N (one greater than the
// maximum n you could ask for at runtime), and may supply a Min value
// (the lowest you would ask for).  You can optionally supply a return
// type R, or if R is auto_t, it will infer the return type.
//
// Make sure f takes the std::integral_constant argument by value and
// not by reference so that it can convert to a compile-time constant.
template<size_t Max, size_t Min = 0, typename R = auto_t, typename F>
requires (Max >= Min)
inline decltype(auto)
with_n(size_t n, F &&f)
{
  return [n, &f]<size_t... I>(std::index_sequence<I...>) -> decltype(auto) {
    using return_type =
        std::conditional_t<std::same_as<R, auto_t>,
                           merged_return_t<decltype(std::forward<F>(f)(
                               std::integral_constant<size_t, I + Min>{}))...>,
                           R>;
    constexpr std::array<return_type (*)(F &&), Max - Min> invokers{
        +[](F &&f) -> return_type {
          return std::forward<F>(f)(std::integral_constant<size_t, I + Min>{});
        }...};
    return invokers.at(n - Min)(std::forward<F>(f));
  }(std::make_index_sequence<Max - Min>{});
}

namespace detail {
// What you should do when spinning (e.g., to give other hardware
// threads priority).
inline void
pause_instruction()
{
#if HAVE_BUILTIN_IA32_PAUSE
  __builtin_ia32_pause();
#elif HAVE_BUILTIN_ARM_YIELD
  // XXX - only works on clang?
  __builtin_arm_yield();
#endif // HAVE_BUILTIN_ARM_YIELD
}
} // namespace detail

// Note: this could break binary compatibility, but we don't have that
// across compilers anyway because of how clang deals with
// [[no_unique_address]]:
// https://github.com/llvm/llvm-project/issues/50766
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wunknown-warning-group"
#pragma GCC diagnostic ignored "-Winterference-size"
constexpr size_t hardware_destructive_interference_size =
    std::hardware_destructive_interference_size;
#pragma GCC diagnostic pop

} // namespace x23
