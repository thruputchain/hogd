#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

#include "hogd/syscall/syscall_status.h"

namespace hogd {

struct GasAmount {
  uint64_t gas;
  constexpr explicit GasAmount(uint64_t amt) : gas(amt) {}

  auto &operator+=(this auto &self, GasAmount const &other)
  {
    self.gas += other.gas;
    return self;
  }

  bool operator==(GasAmount const &other) const = default;
};

constexpr GasAmount
operator""_gas(uint64_t input)
{
  return GasAmount{input};
};

// like std expected, but metered
template<typename T> struct MeteredResult {
  GasAmount gas_consumed;
  SyscallStatus<T> result;

  template<typename... Args>
  MeteredResult(GasAmount gas, Args &&...args)
    : gas_consumed(gas), result(std::forward<Args>(args)...)
  {}

  MeteredResult() : gas_consumed(0), result() {}

  auto and_then(this auto self, auto lambda)
  {
    using return_t = decltype([] {
      if constexpr (std::same_as<void, T>) {
        return decltype(lambda())();
      }
      else {
        return decltype(lambda(*self.result))();
      }
    }()
                                  .result)::value_type;

    if (!self.result) {
      return MeteredResult<return_t>(self.gas_consumed, self.result.error());
    }
    if constexpr (std::same_as<T, void>) {
      auto res = lambda();
      res.gas_consumed += self.gas_consumed;
      return res;
    }
    else {
      auto res = lambda(*self.result);
      res.gas_consumed += self.gas_consumed;
      return res;
    }
  }

  auto operator*(this auto self)
  {
    if constexpr (std::same_as<void, T>) {
      return;
    }
    else {
      return *self.result;
    }
  }

  auto operator->(this auto self) { return self.result; }

  operator bool() const { return result.has_value(); }

  friend auto operator>>(MeteredResult<T> &&base, auto lambda)
  {
    return std::forward<MeteredResult<T>>(base).and_then(lambda);
  }

  friend auto operator>>(MeteredResult<T> const &base, auto lambda)
  {
    return base.and_then(lambda);
  }
};

} // namespace hogd
