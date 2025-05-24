#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
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

template<typename ContractTrait> struct ContractTraitMeta {

  template<size_t I>
  using signature = decltype(ContractTrait::template export_types<I>());

  // map the i'th function name to the sorted index that it's actually exported
  // at
  template<size_t I> requires (I < ContractTrait::kExportNames.size())
  constexpr static size_t export_index()
  {
    size_t out = 0;
    auto name = ContractTrait::kExportNames[I];
    for (size_t i = 0; i < ContractTrait::kExportNames.size(); ++i) {
      if (ContractTrait::kExportNames[i] < name) {
        ++out;
      }
    }
    return out;
  }

  constexpr static std::array<size_t, ContractTrait::kExportNames.size()>
  index_map()
  {
    return []<std::size_t... Is>(std::index_sequence<Is...>) {
      return std::array<size_t, sizeof...(Is)>{export_index<Is>()...};
    }(std::make_index_sequence<ContractTrait::kExportNames.size()>{});
  }

  template<StringLiteral Name> constexpr static size_t lookup_index()
  {
    return []<size_t I, std::size_t... Is>(this auto self,
                                           std::index_sequence<I, Is...>) {
      if constexpr (ContractTrait::kExportNames[I] == Name) {
        return I;
      }
      else {
        return self(std::index_sequence<Is...>{});
      }
    }(std::make_index_sequence<ContractTrait::kExportNames.size()>{});
  }

  template<size_t I> using ordered_signature = signature<export_index<I>>;

  template<StringLiteral Name> constexpr static MethodID method_id()
  {
    return sdk_method_id(ContractTrait::kTraitName, ContractTrait::kVersion,
                         index_map()[lookup_index<Name>()]);
  }
};

} // namespace hogd
