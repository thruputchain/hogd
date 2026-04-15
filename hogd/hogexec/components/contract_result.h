#pragma once

#include <concepts>
#include <cstring>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "x23/vec.h"

namespace hogd {

template<typename T>
concept is_contract_exception = requires {
  {T::selector} -> std::same_as<uint64_t>;
};

inline uint64_t
calculate_selector(std::string_view major_version, std::string_view crate, std::string_view type_name)
{
  x23::Buf buf;
  buf.append_range(major_version);
  buf.append_range("::");
  buf.append_range(crate);
  buf.append_range("::");
  buf.append_range(type_name);
  auto hash = sha256(buf);
  uint64_t out = 0;
  std::memcpy(&out, hash.data(), sizeof(out));
  return out;
}

struct LibraryExn {
  const uint64_t selector = calculate_selector("1", "LibraryExn", "sdk");
};

template<is_contract_exception... Exns>
class ContractException {
public:


private:
  std::variant<LibraryExn, Exns...> variant_;
};

// Parse and work with result types returned by contract methods.
template<typename T, typename E> class Result {
public:
  constexpr static bool kIsContractResultType = true;

  using ok_type = std::conditional_t<std::is_same_v<T, void>, std::tuple<>, T>;
  using err_type = std::conditional_t<std::is_same_v<E, void>, std::tuple<>, E>;

  struct OkType {
    ok_type value_;
  };

  template<typename... Args> requires std::is_constructible_v<ok_type, Args...>
  static Result make_ok(Args &&...args)
  {
    return Result(OkType(std::forward<Args>(args)...));
  }

  template<typename... Args> requires std::is_constructible_v<err_type, Args...>
  static Result make_err(Args &&...args)
  {
    return Result(err_type(std::forward<Args>(args)...));
  }

  Result(OkType &&t) : variant_(std::move(t)) {}
  Result(err_type e) : variant_(std::move(e)) {}

  auto const &ok() const { return std::get<OkType>(variant_).value_; }
  auto const &err() const { return std::get<err_type>(variant_); }

  bool is_ok() const { return std::holds_alternative<OkType>(variant_); }
  bool is_err() const { return std::holds_alternative<err_type>(variant_); }

private:
  static_assert(!std::is_same_v<OkType, err_type>);
  std::variant<OkType, err_type> variant_;
};

template<typename T>
concept is_contract_result_type = requires {
  { T::kIsContractResultType } -> std::convertible_to<bool>;
};

static_assert(is_contract_result_type<Result<void, void>>);
static_assert(is_contract_result_type<Result<void, int32_t>>);
static_assert(is_contract_result_type<Result<int32_t, void>>);
static_assert(is_contract_result_type<Result<int32_t, int32_t>>);

} // namespace hogd
