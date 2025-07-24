#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "contract.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

// Common utilities for building preset contracts

// Get the method ID for a given method,
// at which contracts using the SDK will expect to find it.
MethodID sdk_method_id(std::string_view identifier_name,
                       std::string_view identifier_version, uint16_t method_id);
ContractEntrypointID
sdk_entrypoint_derivation(std::string_view identifier_name,
                          std::string_view identifier_version,
                          uint16_t method_id);
MethodID sdk_method_id(ContractEntrypointID entry_point_id);
ContractEntrypointID entrypoint_from_method_id(MethodID method_id);

namespace detail {

template<typename> struct CalldataArgs;

template<typename R, typename... Args> struct CalldataArgs<R (*&&)(Args...)> {
  using args_t = std::tuple<Args...>;
  using return_t = std::conditional_t<std::is_same_v<R, void>, std::tuple<>, R>;
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
  template<StringLiteral Name>
  constexpr static size_t kLookupIndex =
      []<size_t I, std::size_t... Is>(this auto self,
                                      std::index_sequence<I, Is...>) {
        if constexpr (ContractTrait::kExportNames[I] == Name.get()) {
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
  template<StringLiteral Name>
  constexpr static ContractEntrypointID entrypoint()
  {
    return sdk_entrypoint_derivation(ContractTrait::kTraitName,
                                     ContractTrait::kVersion,
                                     kExportIndex<kLookupIndex<Name>>);
  }

  template<StringLiteral Name, typename... Args>
  static Calldata make_calldata(Args... args) requires (
      std::same_as<
          std::tuple<Args...>,
          typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::args_t>)
  {
    return make_encoding(std::tuple{args...});
  }

  template<StringLiteral Name, typename ReturnType>
  static Calldata make_return(ReturnType return_value) requires (
      std::same_as<ReturnType, typename detail::CalldataArgs<
                                   signature<kLookupIndex<Name>>>::return_t>)
  {
    return make_encoding(return_value);
  }

  // Native contract is responsible for handling errors related to failing to
  // parse calldata. This behavior mirrors the behavior implemented in the SDK.
  template<StringLiteral Name>
  static auto decode_calldata(std::span<const std::byte> data)
  {
    using args_type =
        typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::args_t;
    return [data] -> SyscallStatus<args_type> {
      args_type out;
      try {
        x23::deserialize_from(&out, data);
      } catch (const x23::XdrBadInput &e) {
        return SyscallError{SyscallErrorCode::kCalldataParseError};
      }
      return out;
    }();
  }

  template<StringLiteral Name>
  static auto decode_return(std::span<const std::byte> data)
  {
    using return_type =
        typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::return_t;
    return [data] -> SyscallStatus<return_type> {
      return_type out;
      try {
        x23::deserialize(&out, data);
      } catch (const x23::XdrBadInput &e) {
        return SyscallError{SyscallErrorCode::kReturnParseError};
      }

      return out;
      // if the return type is a single value, return it directly instead of as
      // a tuple
      /*if constexpr (requires { std::tuple_size_v<return_type> == 1; }) {
        return std::get<0>(out);
      }
      else {
        return out;
      } */
    }();
  }

  template<StringLiteral Name>
  using args_t =
      typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::args_t;
  template<StringLiteral Name>
  using return_t =
      typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::return_t;

private:
  template<typename... Args>
  static Calldata make_encoding(std::tuple<Args...> args)
  {
    return x23::serialize(args);
    // auto out = bcs::encode_to<Calldata>(args);
    // if (!out.has_value()) {
    //   throw std::runtime_error("Failed to encode calldata");
    // }
    // return out.value();
  }
};

template<is_contract_trait ContractTrait> class ContractTraitClient {
public:
  ContractTraitClient(ContractAddress contract_address)
    : contract_address_(contract_address)
  {}

  using trait_meta_t = ContractTraitMeta<ContractTrait>;

  template<StringLiteral Name, typename... Args>
  Invocation make_invocation(GasAmount gas_amount, Args &&...args)
  {
    auto calldata =
        trait_meta_t::template make_calldata<Name>(std::forward<Args>(args)...);
    return Invocation{.invoked_address = contract_address_,
                      .entrypoint = trait_meta_t::template entrypoint<Name>(),
                      .gas_limit = gas_amount,
                      .calldata = calldata};
  }

  template<StringLiteral Name, typename... Args>
  auto invoke(SystemCallHandler &handler, Args &&...args)
      -> std::pair<HostFnStatus<SyscallStatus<
                       typename trait_meta_t::template return_t<Name>>>,
                   uint64_t>
  {
    auto calldata =
        trait_meta_t::template make_calldata<Name>(std::forward<Args>(args)...);
    auto res =
        handler.invoke(contract_address_,
                       sdk_method_id(trait_meta_t::template entrypoint<Name>()),
                       UINT64_MAX, calldata, 0);

    HostFnStatus<SyscallErrorCode> host_fn_res = res.first;

    if (!host_fn_res.has_value()) {
      return std::pair{host_fn_res.error(), res.second};
    }
    if (host_fn_res.value() != SyscallErrorCode::kNone) {
      return std::pair{SyscallError{host_fn_res.value()}, res.second};
    }

    auto return_buffer = handler.get_return_buffer();
    if (!return_buffer.has_value()) {
      return std::pair{return_buffer.error(), res.second};
    }
    assert(*return_buffer);
    std::span<const std::byte> return_span = std::as_bytes(
        std::span((*return_buffer)->data(), (*return_buffer)->size()));

    auto return_value = trait_meta_t::template decode_return<Name>(return_span);
    if (!return_value.has_value()) {
      return std::pair{return_value.error(), res.second};
    }
    return std::pair{return_value.value(), res.second};
  }

private:
  const ContractAddress contract_address_;
};

} // namespace hogd
