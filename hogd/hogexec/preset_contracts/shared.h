#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

#include "bcs/decode.h"
#include "bcs/encode.h"
#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// Common utilities for building preset contracts

// Get the method ID for a given method,
// at which contracts using the SDK will expect to find it.
MethodID sdk_method_id(std::string_view identifier_name,
                       std::string_view identifier_version, uint16_t method_id);
MethodID sdk_method_id(ContractEntrypointID entry_point_id);

namespace detail {
// Wrapper to make string literals work as template parameters.
template<size_t N> struct StringLiteral {
  char value[N];

  constexpr StringLiteral(const char (&str)[N])
  {
    for (size_t i = 0; i < N; ++i)
      value[i] = str[i];
  }

  constexpr operator std::string_view() const
  {
    return std::string_view(value, N - 1);
  }
};

template<typename> struct CalldataArgs;

template<typename R, typename... Args> struct CalldataArgs<R (*&&)(Args...)> {
  using args_t = std::tuple<Args...>;
  using return_t = R;
};

} // namespace detail

template<typename T>
concept is_contract_trait = requires {
  { T::kTraitName } -> std::convertible_to<std::string_view>;
  { T::kVersion } -> std::convertible_to<std::string_view>;
  {
    T::kExportNames
  }
  -> std::convertible_to<std::array<std::string_view, T::kExportNames.size()>>;
  {
    std::tuple_size_v<typename T::export_types>
  } -> std::convertible_to<size_t>;
} && requires {
  std::tuple_size_v<typename T::export_types> == T::kExportNames.size();
};

template<is_contract_trait ContractTrait> class ContractTraitMeta {

  // the type of the i'th exported function,
  // (ordered by their listing in ContractTrait::kExportNames)
  template<size_t I>
  using signature = decltype(std::get<I>(
      std::declval<typename ContractTrait::export_types>()));

  // the index of the function with the given name,
  // (ordered by their listing in ContractTrait::kExportNames)
  template<detail::StringLiteral Name>
  constexpr static size_t kLookupIndex =
      []<size_t I, std::size_t... Is>(this auto self,
                                      std::index_sequence<I, Is...>) {
        if constexpr (ContractTrait::kExportNames[I] == Name) {
          return I;
        }
        else {
          return self(std::index_sequence<Is...>{});
        }
      }(std::make_index_sequence<ContractTrait::kExportNames.size()>{});

  // map the i'th function name to the sorted index that it's actually exported
  // at (i.e. the index of the function in the sorted list of exported
  // functions)
  template<size_t I> requires (I < ContractTrait::kExportNames.size())
  constexpr static size_t kExportIndex = [] {
    size_t out = 0;
    auto name = ContractTrait::kExportNames[I];
    for (size_t i = 0; i < ContractTrait::kExportNames.size(); ++i) {
      if (ContractTrait::kExportNames[i] < name) {
        ++out;
      }
    }
    return out;
  }();

public:
  template<detail::StringLiteral Name> constexpr static MethodID method_id()
  {
    return sdk_method_id(ContractTrait::kTraitName, ContractTrait::kVersion,
                         kExportIndex<kLookupIndex<Name>>);
  }

  template<detail::StringLiteral Name, typename... Args>
  static Calldata make_calldata(Args... args) requires (
      std::same_as<
          std::tuple<Args...>,
          typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::args_t>)
  {
    return make_encoding(std::tuple{args...});
  }

  template<detail::StringLiteral Name, typename ReturnType>
  static Calldata make_return(ReturnType return_value) requires (
      std::same_as<ReturnType, typename detail::CalldataArgs<
                                   signature<kLookupIndex<Name>>>::return_t>)
  {
    return make_encoding(return_value);
  }

  // Native contract is responsible for handling errors related to failing to
  // parse calldata. This behavior mirrors the behavior implemented in the SDK.
  template<detail::StringLiteral Name>
  static auto decode_calldata(std::span<const std::byte> data)
  {
    using args_type =
        typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::args_t;
    return [data] -> SyscallStatus<args_type> { 
        auto r = bcs::decode_from<args_type>(data);
        if (!r.has_value()) {
            return SyscallStatus<args_type>(SyscallErrorCode::kCalldataParseError);
        }
        return SyscallStatus<args_type>(r.value());
    }();
  }

  template<detail::StringLiteral Name>
  static auto decode_return(std::span<const std::byte> data)
  {
    using return_type =
        typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::return_t;
    return [data] -> SyscallStatus<return_type> { 
        auto r = bcs::decode_from<return_type>(data);
        if (!r.has_value()) {
            return SyscallError{SyscallErrorCode::kReturnParseError};
        }
        // if the return type is a single value, return it directly instead of as a tuple
        if constexpr (requires { std::tuple_size_v<return_type> == 1; }) {
            return std::get<0>(r.value());
        } else {
            return r.value();
        }
    }();
  }

private:
  template<typename... Args>
  static Calldata make_encoding(std::tuple<Args...> args)
  {
    auto out = bcs::encode_to<Calldata>(args);
    if (!out.has_value()) {
      throw std::runtime_error("Failed to encode calldata");
    }
    return out.value();
  }
};

} // namespace hogd
