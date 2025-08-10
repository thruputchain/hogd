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

  using ok_type = std::conditional_t<std::is_same_v<T, void>, std::tuple<>, T>;
  using err_type = E;

  Result(ok_type const &t) : variant_(t) {}
  Result(err_type const &e) : variant_(e) {}

  auto const &ok() const { return std::get<ok_type>(variant_); }
  auto const &err() const { return std::get<err_type>(variant_); }

  bool is_ok() const { return std::holds_alternative<ok_type>(variant_); }
  bool is_err() const { return std::holds_alternative<err_type>(variant_); }

private:
  std::variant<ok_type, err_type> variant_;
};

template<typename T>
concept is_contract_result_type = requires {
  { T::kIsContractResultType } -> std::convertible_to<bool>;
};

static_assert(is_contract_result_type<Result<void>>);

} // namespace hogd
