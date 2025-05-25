#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

#include "contract.hh"
#include "external/bcs-cpp/bcs/encode.h"
#include "hogd/base/types.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// Common utilities for building preset contracts

// Get the method ID for a given method,
// at which contracts using the SDK will expect to find it.
MethodID sdk_method_id(std::string_view identifier_name,
                       std::string_view identifier_version, uint16_t method_id);
MethodID sdk_method_id(ContractEntrypointID entry_point_id);

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

template<typename ContractTrait> class ContractTraitMeta {
  template<size_t I>
  using signature = decltype(std::get<I>(
      std::declval<typename ContractTrait::export_types>()));

  template<StringLiteral Name>
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
  // at
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
  template<StringLiteral Name> constexpr static MethodID method_id()
  {
    return sdk_method_id(ContractTrait::kTraitName, ContractTrait::kVersion,
                         kExportIndex<kLookupIndex<Name>>);
  }

  template<StringLiteral Name, typename... Args>
  static Calldata make_calldata(Args... args)
      requires (std::same_as<
                std::tuple<Args...>,
                typename CalldataArgs<signature<kLookupIndex<Name>>>::args_t>)
  {
    return make_encoding(std::tuple{args...});
  }

  template<StringLiteral Name, typename ReturnType>
  static Calldata make_return(ReturnType return_value)
      requires (std::same_as<
                ReturnType,
                typename CalldataArgs<signature<kLookupIndex<Name>>>::return_t>)
  {
    return make_encoding(return_value);
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
