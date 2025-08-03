#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>

#include "bcs/decode.h"
#include "bcs/encode.h"
#include "hogd/base/log.h"
#include "transaction.hh"
#include "x23/serialize.h"
#include "x23/serialize_cxs.h"
#include "contract_error.hh"
#include "x23/utility.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace detail {

template<typename T>
struct Result {
  Result() noexcept : Result(false) {}
  explicit Result(bool d) noexcept {
      using ops = x23::union_ops<struct Result<T>>;
      ops::construct(*this, d);
  }
  Result(Result && other) noexcept {
    using ops = x23::union_ops<struct Result<T>>;
    ops::construct(*this, std::move(other));
  }
  Result(const Result & other) {
    using ops = x23::union_ops<struct Result<T>>;
    ops::construct(*this, other);
  }
  ~Result() {
    using ops = x23::union_ops<struct Result<T>>;
    ops::destroy(*this);
  }
  Result &operator=(Result &&other) noexcept {
    using ops = x23::union_ops<struct Result<T>>;
    ops::assign(*this, std::move(other));
    return *this;
  }
  Result &operator=(const Result &other) {
    using ops = x23::union_ops<struct Result<T>>;
    ops::assign(*this, other);
    return *this;
  }

  // get and set discriminant
  bool is_ok() const noexcept { return __xdr_union_field_is_ok; }
  Result &is_ok(bool d) noexcept
  {
    using ops = x23::union_ops<struct Result<T>>;
    ops::set_discriminant(*this, d);
    return *this;
  }

  // case true:
  //   ErrorXDR &err(); // FIELD 2
  // case false:
  //   T &value();  // FIELD 1

  template<typename Self> decltype(auto) err(this Self &&self)
  {
    return get<2>(std::forward<Self>(self));
  }
  template<typename Self> decltype(auto) value(this Self &&self)
  {
    return get<1>(std::forward<Self>(self));
  }

  friend bool operator==(const Result & a, const Result & b) {
    return x23::union_ops<struct Result<T>>::eq(a, b);
  }
  friend std::partial_ordering
  operator<=>(const Result & a, const Result & b) {
    return x23::union_ops<struct Result<T>>::cmp(a, b);
  }
  // NOLINTBEGIN(bugprone-reserved-identifier)
  // NOLINTBEGIN(readability-identifier-naming)
  [[gnu::used]] static constexpr int __xdr_union_arm_number(bool discriminant)
   {  
      if (discriminant) {
        return 2;
      }
      return 1;
    }
  
private:
  bool __xdr_union_field_is_ok;
  union {
    ErrorXDR __xdr_union_field_err;
    T __xdr_union_field_x;
  };

  template<size_t I> requires (I <= 2)
  static constexpr auto __xdr_union_fieldptr()
  {
    if constexpr (I == 0)
      return &Result<T>::__xdr_union_field_is_ok;
    else if constexpr (I == 1)
      return &Result<T>::__xdr_union_field_x;
    else if constexpr (I == 2)
      return &Result<T>::__xdr_union_field_err;
  }

  template<x23::is_xdr_union U> friend struct x23::union_ops;

  template<std::size_t I, x23::references<Result<T>> U> requires (I <= 2)
  friend decltype(auto) get(U &&u) {
    using ops = x23::union_ops<struct Result<T>>;
    if constexpr (I == 0)
      return ops::get_discriminant(u);
    else
      return ops::template get<I>(std::forward<U>(u));
  }
};
} // namespace detail

template<typename T>
using Result = detail::Result<std::conditional_t<std::is_same_v<T, void>, VoidXDR, T>>;

// NOLINTEND(readability-identifier-naming)
// NOLINTEND(bugprone-reserved-identifier)
} // namespace hogd

namespace x23 {

template<typename T>
struct xdr_meta<struct hogd::detail::Result<T>> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kUnion;
  static constexpr const char *type_name = "Result_T";
  static constexpr int default_field = -1;
  static constexpr bool has_void = false;
  static constexpr std::array field_names{
    "is_ok", // discriminant
    "x",
    "err",
  };
  using discriminant_type =
    decltype(std::declval<struct hogd::detail::Result<T>>().is_ok());
  //using enum x23::AlwaysEnum<discriminant_type>::type;
  static constexpr
  std::array<std::pair<discriminant_type, int>, 2> cases{
    std::pair{false, 1},
    std::pair{true, 2},
  };
};
  /*

template<typename T>
struct CxsSerializer<std::variant<T, hogd::ErrorXDR>> {

  static void store(Buf &buf, hogd::Result<T> value)
  {
    if (std::holds_alternative<T>(value)) {
      CxsSerializer<bool>::store(buf, true);
      CxsSerializer<T>::store(buf, std::get<T>(value));
    } else {
      CxsSerializer<bool>::store(buf, false);
      CxsSerializer<hogd::ErrorXDR>::store(buf, std::get<hogd::ErrorXDR>(value));
    }
  }
  static void load(std::span<const uint8_t> &data, hogd::Result<T> &value)
  {
    bool is_ok;
    CxsSerializer<bool>::load(data, is_ok);
    if (is_ok) {
      T t;
      CxsSerializer<T>::load(data, t);
      value = t;
    } else {
      hogd::ErrorXDR err;
      CxsSerializer<hogd::ErrorXDR>::load(data, err);
      value = err;
    }
  }
};

template<typename T>
struct xdr_meta<std::variant<T, hogd::ErrorXDR>> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kUnion;
  static constexpr const char *type_name = "Result_Generic";
  static constexpr int default_field = -1;
  static constexpr bool has_void = false;
  static constexpr std::array field_names{
    "ok", // discriminant
    "err"
  };
  using discriminant_type = bool;
  using enum x23::AlwaysEnum<discriminant_type>::type;
  static constexpr
  std::array<std::pair<discriminant_type, int>, 2> cases{
    std::pair{false, 1},
    std::pair{true, 2},
  };
};

static_assert(category<hogd::Result<int>> == x23::XdrCategory::kUnion);
static_assert(is_xdr_union<hogd::Result<void>>);
static_assert(!is_xdr_struct<hogd::Result<void>>);
*/

} // namespace x23

namespace hogd {

// Group methods for de/serializing calldata into once location, with a common
// interface.
enum class SerializationFormat { kBcs, kTxs, kCxs };

namespace detail {
template<typename... Args>
using deserialize_return_t = std::optional<std::conditional_t<
    sizeof...(Args) == 1,
    std::tuple_element_t<0, std::tuple<std::remove_cvref_t<Args>...>>,
    std::tuple<std::remove_cvref_t<Args>...>>>;
} // namespace detail

template<SerializationFormat Format> struct Serializer;

template<> struct Serializer<SerializationFormat::kBcs> {

  template<typename... Args>
  static std::optional<Calldata> serialize(Args const &...args)
  {
    auto r = bcs::encode_to<Calldata>(args...);
    if (!r.has_value()) {
      return std::nullopt;
    }
    return r.value();
  }

  template<typename... Args>
  static auto deserialize(std::span<const std::byte> data)
      -> detail::deserialize_return_t<Args...>
  {
    if constexpr (sizeof...(Args) == 1) {
      auto r = bcs::decode_from<Args...>(data);
      if (!r.has_value()) {
        return std::nullopt;
      }
      return std::get<0>(r.value());
    }
    else {
      auto r = bcs::decode_from<std::tuple<std::remove_cvref_t<Args>...>>(data);
      if (!r.has_value()) {
        return std::nullopt;
      }
      // the bcs library is a bit weird, and returns a tuple of tuples
      // TODO(geoff): fix, eventually, if we use bcs
      // There might be a way to avoid this using the parameter packs,
      // but the error handling gets messy and this works
      return std::get<0>(std::get<0>(r.value()));
    }
  }
};

template<> struct Serializer<SerializationFormat::kTxs> {
  template<typename... Args>
  static std::optional<Calldata> serialize(Args const &...args)
  {
    try {
      return x23::serialize(args...);
    } catch (const x23::XdrBadInput &e) {
      trace("TXS Serialize failed: {}", e.what());
      return std::nullopt;
    }
  }

  template<typename... Args>
  static auto deserialize(std::span<const std::byte> data)
      -> detail::deserialize_return_t<Args...>
  {
    auto args = std::tuple<std::remove_cvref_t<Args>...>{};
    try {
      x23::deserialize(&args, data);
    } catch (const x23::XdrBadInput &e) {
      trace("TXS Deserialize failed: {}", e.what());
      return std::nullopt;
    }

    if constexpr (sizeof...(Args) == 1) {
      return std::get<0>(args);
    }
    else {
      return args;
    }
  }
};

template<> struct Serializer<SerializationFormat::kCxs> {
  template<typename... Args>
  static std::optional<Calldata> serialize(Args const &...args)
  {
    try {
      return x23::serialize_cxs(args...);
    } catch (const x23::XdrBadInput &e) {
      trace("CXS Serialize failed: {}", e.what());
      return std::nullopt;
    }
  }

  template<typename... Args>
  static auto deserialize(std::span<const std::byte> data)
      -> detail::deserialize_return_t<Args...>
  {
    auto args = std::tuple<std::remove_cvref_t<Args>...>{};
    try {
      x23::deserialize_cxs(&args,
                           {reinterpret_cast<const uint8_t *>(data.data()),
                            data.size()});
    } catch (const x23::XdrBadInput &e) {
      trace("CXS Deserialize failed: {}", e.what());
      return std::nullopt;
    }
    if constexpr (sizeof...(Args) == 1) {
      return std::get<0>(args);
    }
    else {
      return args;
    }
  }
};

} // namespace hogd
