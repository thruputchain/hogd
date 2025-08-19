#pragma once

#include <concepts>
#include <tuple>
#include <type_traits>
#include <variant>

#include "contract_error.hh"

namespace hogd {

// Parse and work with result types returned by contract methods.
template<typename T, typename E = ErrorXDR> class Result {
public:
  constexpr static bool kIsContractResultType = true;

  template<typename S> struct OkType;

  template<typename S> requires (!std::is_void_v<S>)
  struct OkType<S> {
    S value_;
  };

  template<> struct OkType<void> {
    std::tuple<> value_;
  };

  using ok_type = std::conditional_t<std::is_same_v<T, void>, std::tuple<>, T>;
  using err_type = std::conditional_t<std::is_same_v<E, void>, std::tuple<>, E>;

  template<typename... Args> requires std::is_constructible_v<ok_type, Args...>
  static Result make_ok(Args &&...args)
  {
    return Result(OkType<T>(std::forward<Args>(args)...));
  }

  template<typename... Args> requires std::is_constructible_v<err_type, Args...>
  static Result make_err(Args &&...args)
  {
    return Result(err_type(std::forward<Args>(args)...));
  }

  Result(OkType<T> &&t) : variant_(std::move(t)) {}
  Result(err_type e) : variant_(std::move(e)) {}

  auto const &ok() const { return std::get<OkType<T>>(variant_).value_; }
  auto const &err() const { return std::get<err_type>(variant_); }

  bool is_ok() const { return std::holds_alternative<OkType<T>>(variant_); }
  bool is_err() const { return std::holds_alternative<err_type>(variant_); }

private:
  static_assert(!std::is_same_v<OkType<T>, err_type>);
  std::variant<OkType<T>, err_type> variant_;
};

template<typename T>
concept is_contract_result_type = requires {
  { T::kIsContractResultType } -> std::convertible_to<bool>;
};

static_assert(is_contract_result_type<Result<void>>);

} // namespace hogd
