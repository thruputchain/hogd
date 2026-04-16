#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/components/library_exception.hh"
#include "hogd/xdr/transaction.hh"
#include "x23/serialize.h"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"

namespace hogd {

template<typename T, uint64_t Selector> struct ContractException {
  constexpr static uint64_t kSelector = Selector;
  T exn;
};

inline uint64_t
calculate_selector(std::string_view major_version, std::string_view crate,
                   std::string_view type_name)
{
  x23::Buf buf;
  buf.append_range(major_version);
  buf.append_range(std::string_view("::"));
  buf.append_range(crate);
  buf.append_range(std::string_view("::"));
  buf.append_range(type_name);
  auto hash = sha256(buf);
  // First 8 digest bytes as big-endian u64 (matches contract SDK).
  uint64_t out = 0;
  static_assert(std::endian::native == std::endian::little);
  std::memcpy(&out, hash.data(), sizeof(out));
  out = std::byteswap(out);
  return out;
}

// SHA256("1::sdk::LibraryExn")[0..8] as big-endian u64 (sdk_rs
// exception_selector).
inline constexpr uint64_t kLibraryExceptionSelector = 0xe562d59059c60ac3ULL;

using LibraryException =
    ContractException<LibraryExn, kLibraryExceptionSelector>;

template<typename... T> struct ContractErrorType {
public:
  ContractErrorType(ReturnBuffer &&abort_buffer)
  {
    CHECK_EQ(kLibraryExceptionSelector,
             calculate_selector("1", "sdk", "LibraryExn"));
    try {
      uint64_t selector = 0;
      size_t used_bytes =
          x23::deserialize_cxs_from(&selector, x23::BufViewEnd{abort_buffer});

      std::span<const uint8_t> const body_span(abort_buffer.data() + used_bytes,
                                               abort_buffer.size() -
                                                   used_bytes);

      bool matched =
          this->template try_dispatch_exception<LibraryException>(selector,
                                                                  body_span);
      if constexpr (sizeof...(T) > 0) {
        matched =
            matched ||
            (this->template try_dispatch_exception<T>(selector, body_span) ||
             ...);
      }
      if (!matched) {
        variant_ = ParseFailureType{std::move(abort_buffer)};
      }
    } catch (const x23::XdrException &) {
      variant_ = ParseFailureType{std::move(abort_buffer)};
    }
  }

  template<typename U>
  requires (std::is_same_v<std::remove_cvref_t<U>, LibraryException> ||
            (sizeof...(T) > 0 &&
             (std::is_same_v<std::remove_cvref_t<U>, T> || ...)))
  ContractErrorType(U &&exn) : variant_(std::forward<U>(exn))
  {}

private:
  struct ParseFailureType {
    ReturnBuffer abort_buffer;
  };

  // `U` is `ContractException<Exn, Sel>` with `kSelector` and `exn`.
  template<typename U>
  bool try_dispatch_exception(uint64_t selector, std::span<const uint8_t> body)
  {
    if (selector != U::kSelector) {
      return false;
    }
    std::remove_cvref_t<decltype(std::declval<U>().exn)> exn{};
    x23::deserialize_cxs(&exn, body);
    variant_ = U{exn};
    return true;
  }

public:
  // Re-serialize to (u64 selector, CXS body) format.
  ReturnBuffer serialize() const
  {
    return std::visit(
        [](auto const &v) -> ReturnBuffer {
          using V = std::remove_cvref_t<decltype(v)>;
          if constexpr (std::is_same_v<V, ParseFailureType>) {
            return v.abort_buffer;
          }
          else {
            // V is a ContractException<Exn, Sel>
            x23::Buf buf = x23::serialize_cxs(V::kSelector);
            buf.append_range(x23::serialize_cxs(v.exn));
            return ReturnBuffer(buf.begin(), buf.end());
          }
        },
        variant_);
  }

  std::variant<LibraryException, ParseFailureType, T...> variant_;
};

// Parse and work with result types returned by contract methods.
template<typename T, typename... Exns> class Result {
public:
  constexpr static bool kIsContractResultType = true;

  using ok_type = std::conditional_t<std::is_same_v<T, void>, std::tuple<>, T>;
  using err_type = ContractErrorType<Exns...>;

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

static_assert(is_contract_result_type<Result<void>>);

} // namespace hogd
