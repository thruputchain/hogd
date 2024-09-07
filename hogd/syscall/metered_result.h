#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

#include "hogd/syscall/syscall_status.h"

namespace hogd {

//like std expected, but metered
template<typename T> 
struct MeteredResult
{
  SyscallStatus<T> result;
  uint64_t gas_consumed;

  auto chain(auto lambda) {
    using ret_t = decltype(lambda(*result).result)::value_type;
    if (result) {
      auto res = lambda(*result);
      res.gas_consumed += gas_consumed;
      return MeteredResult<ret_t>(std::move(res.result), gas_consumed + res.gas_consumed);
    }
    return MeteredResult<ret_t>{result.error(), gas_consumed};
  }

  friend auto operator>>(MeteredResult<T>&& base, auto lambda)
  {
    return std::forward<MeteredResult<T>>(base).chain(lambda);
  }
};

}

/*

template<> struct MeteredResult<void> {
  using result_t = void;

  uint64_t gas_consumed;
  SyscallError error_code = SyscallError::NONE;

  bool has_error() const { return error_code != SyscallError::NONE; }

  void unwrap()
  {
    if (error_code != SyscallError::NONE) {
      throw std::runtime_error("assertion failed: " +
                               std::to_string(std::to_underlying(error_code)));
    }
    return;
  }

  auto chain(auto lambda)
  {
    using out_t = typename decltype(lambda())::result_t;

    if (has_error()) {
      return static_cast<MeteredResult<out_t>>(
                 error(error_code)) >>
             add_gas(gas_consumed);
    }
    auto res = lambda();
    res.gas_consumed += gas_consumed;
    return res;
  }

  friend auto operator>>(MeteredResult<void> &&base, auto lambda)
  {
    return std::forward<MeteredResult<void>>(base).chain(lambda);
  }

  friend auto operator>>(MeteredResult<void> &&base,
                         detail::add_gas_t const &gas)
  {
    base.gas_consumed += gas.gas;
    return std::forward<MeteredResult<void>>(base);
  }

  friend auto operator>>(MeteredResult<void> &&base,
                         detail::check_gas_t const &gas)
  {
    if (base.gas_consumed >= gas.gas_limit) {
      return MeteredResult<void>{gas.gas_limit, SyscallError::GAS_EXCEEDED};
    }
    return std::forward<MeteredResult<void>>(base);
  }

  friend auto operator>>(MeteredResult<void> &&base, detail::success_t const &)
  {
    return std::forward<MeteredResult<void>>(base);
  }

  friend auto operator>>(MeteredResult<void> &&base,
                         detail::throw_err_t const &err)
  {
    return MeteredResult<void>(base.gas_consumed, err.err);
  }

  MeteredResult<void> &operator>>=(auto lambda)
  {
    *this = chain(lambda);
    return *this;
  }
};

template<typename T> struct MeteredResult {
  std::optional<T> result;
  using result_t = T;
  uint64_t gas_consumed;
  SyscallError error_code = SyscallError::NONE;

  auto chain(auto lambda)
  {
    using out_t = typename decltype(lambda(*result))::result_t;
    if (has_error()) {
      return static_cast<MeteredResult<out_t>>(
                 error(error_code)) >>
             add_gas(gas_consumed);
    }

    if (!result.has_value()) {
      return static_cast<MeteredResult<out_t>>(
                 error(SyscallError::METERED_RESULT_ENCOUNTERED_NULLOPT)) >>
             add_gas(gas_consumed);
    }
    auto res = lambda(*result);
    res.gas_consumed += gas_consumed;
    return res;
  }

  bool has_error() const { return error_code != SyscallError::NONE; }

  T unwrap()
  {
    if (error_code != SyscallError::NONE || (!result.has_value())) {
      throw std::runtime_error("assertion failed: " +
                               std::to_string(std::to_underlying(error_code)));
    }
    return *result;
  }

  friend auto operator>>(MeteredResult<T> &&base, auto lambda)
  {
    return std::forward<MeteredResult<T>>(base).chain(lambda);
  }

  friend auto operator>>(MeteredResult<T> &&base, detail::add_gas_t const &gas)
  {
    base.gas_consumed += gas.gas;
    return std::forward<MeteredResult<T>>(base);
  }

  friend auto operator>>(MeteredResult<T> &&base,
                         detail::check_gas_t const &gas)
  {
    if (base.gas_consumed >= gas.gas_limit) {
      return MeteredResult<T>{std::nullopt, gas.gas_limit,
                              SyscallError::GAS_EXCEEDED};
    }
    return std::forward<MeteredResult<T>>(base);
  }

  friend auto operator>>(MeteredResult<T> &&base,
                         detail::success_t const &success)
  {
    return MeteredResult<void>{base.gas_consumed, base.error_code};
  }

  friend auto operator>>(MeteredResult<T> &&base,
                         detail::throw_err_t const &err)
  {
    return MeteredResult<T>(std::nullopt, base.gas_consumed, err.err);
  }
};

template<typename T>
MeteredResult<T>
operator>>(detail::add_gas_t gas, auto lambda)
{
  return (MeteredResult<void>)(gas) >> lambda;
}

namespace detail {

struct success_t {
  operator MeteredResult<void>() { return MeteredResult<void>{0}; }
};

template<typename T> convertible_err_t::operator MeteredResult<T>()
{
  return MeteredResult<T>(std::nullopt, gas, err);
}

} // namespace detail

detail::success_t success();

template<typename T>
auto
passthrough(auto lambda)
{
  static_assert(std::is_same<decltype(lambda()), MeteredResult<void>>::value,
                "mismatch");

  return [=](T const &val) -> MeteredResult<T> {
    return lambda() >> [&] -> MeteredResult<T> { return {val, 0}; };
  };
}

detail::check_gas_t check_gas(uint64_t limit);

} // namespace marmot

*/