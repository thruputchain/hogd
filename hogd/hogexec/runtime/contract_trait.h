#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "contract.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_result.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "serialize_calldata.h"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/format.h"

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

constexpr static std::array<std::pair<std::string_view, SerializationFormat>, 3>
    kSerializationFormatLookup = {
        std::pair{"txs", SerializationFormat::kTxs},
        std::pair{"cxs", SerializationFormat::kCxs},
        std::pair{"bcs", SerializationFormat::kBcs},
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

  constexpr static size_t
  get_serialization_format_index(std::string_view format)
  {
    for (size_t i = 0; i < detail::kSerializationFormatLookup.size(); ++i) {
      if (detail::kSerializationFormatLookup[i].first == format) {
        return i;
      }
    }
    return std::numeric_limits<size_t>::max();
  }

  constexpr static SerializationFormat get_format_or_default()
  {
    if constexpr (requires {
                    {
                      ContractTrait::kSerializationFormat
                    } -> std::convertible_to<std::string_view>;
                  }) {
      constexpr auto kIndex =
          get_serialization_format_index(ContractTrait::kSerializationFormat);
      static_assert(kIndex != std::numeric_limits<size_t>::max(),
                    "Invalid serialization format");
      return detail::kSerializationFormatLookup[kIndex].second;
    }
    else {
      return SerializationFormat::kTxs;
    }
  }

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
  constexpr static SerializationFormat kSerializationFormat =
      get_format_or_default();

  template<StringLiteral Name> static ContractEntrypointID entrypoint()
  {
    constexpr auto kIndex = kExportIndex<kLookupIndex<Name>>;

    auto entrypoint =
        sdk_entrypoint_derivation(ContractTrait::kTraitName,
                                  ContractTrait::kVersion,
                                  kExportIndex<kLookupIndex<Name>>);
    if constexpr (requires { ContractTrait::kEntrypoints; }) {
      assert(ContractTrait::kEntrypoints[kIndex] == sdk_method_id(entrypoint));
    }
    return entrypoint;
  }

  template<StringLiteral Name, typename... Args>
  static Calldata make_calldata(Args const &...args) requires (
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
      auto out =
          Serializer<kSerializationFormat>::template deserialize<args_type>(
              data);
      if (!out.has_value()) {
        return SyscallError{SyscallErrorCode::kCalldataParseError};
      }
      return *out;
    }();
  }

  template<StringLiteral Name>
  static auto decode_return(std::span<const std::byte> data,
                            bool manually_reverted)
  {
    using return_type =
        typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::return_t;
    return [data, manually_reverted] -> SyscallStatus<return_type> {
      if constexpr (is_contract_result_type<return_type>) {
        if (manually_reverted) {
          auto out = Serializer<kSerializationFormat>::template deserialize<
              typename return_type::err_type>(data);
          if (!out.has_value()) {
            return SyscallError{SyscallErrorCode::kReturnParseError};
          }
          return return_type::make_err(*out);
        }
        // ok type
        auto out = Serializer<kSerializationFormat>::template deserialize<
            typename return_type::ok_type>(data);
        if (!out.has_value()) {
          return SyscallError{SyscallErrorCode::kReturnParseError};
        }
        return return_type::make_ok(*out);
      }
      else {

        // not using the manual revert path

        if (manually_reverted) {
          return SyscallError{SyscallErrorCode::kFrameReverted};
        }

        auto out =
            Serializer<kSerializationFormat>::template deserialize<return_type>(
                data);
        if (!out.has_value()) {
          return SyscallError{SyscallErrorCode::kReturnParseError};
        }
        return *out;
      }
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
  static Calldata make_encoding(std::tuple<Args...> const &args)
  {
    auto out = Serializer<kSerializationFormat>::serialize(args);
    if (!out.has_value()) {
      throw std::runtime_error("Failed to encode calldata");
    }
    return out.value();
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

  template<StringLiteral Name, InvokeFlagSet Flags = std::to_underlying(
                                   InvokeFlag::kInvokeFlagNone)>
  auto invoke(SystemCallHandler &handler, auto &&...args)
      -> std::pair<HostFnStatus<SyscallStatus<
                       typename trait_meta_t::template return_t<Name>>>,
                   uint64_t>
  {
    auto calldata = trait_meta_t::template make_calldata<Name>(
        std::forward<decltype(args)>(args)...);
    trace("Invoke method {} (Name {}) calldata: {}",
          sdk_method_id(trait_meta_t::template entrypoint<Name>()), Name.get(),
          x23::show(calldata));
    auto res =
        handler.invoke(contract_address_,
                       sdk_method_id(trait_meta_t::template entrypoint<Name>()),
                       UINT64_MAX, calldata, Flags);

    HostFnStatus<SyscallErrorCode> host_fn_res = res.first;

    if (!host_fn_res.has_value()) {
      trace("Invoke HostFnStatus error: {}", host_fn_res.error());
      return std::pair{host_fn_res.error(), res.second};
    }

    if (host_fn_res.value() != SyscallErrorCode::kNone &&
        host_fn_res.value() != SyscallErrorCode::kFrameReverted) {
      trace("Invoke SyscallStatus error: {}", host_fn_res.value());
      return std::pair{SyscallError{host_fn_res.value()}, res.second};
    }

    auto return_buffer = handler.get_return_buffer();
    if (!return_buffer.has_value()) {
      trace("No return buffer available: {}", return_buffer.error());
      return std::pair{return_buffer.error(), res.second};
    }
    assert(*return_buffer);
    std::span<const std::byte> return_span = std::as_bytes(
        std::span((*return_buffer)->data(), (*return_buffer)->size()));

    trace("Received return buffer: {}", x23::show(**return_buffer));

    bool manually_reverted =
        host_fn_res.value() == SyscallErrorCode::kFrameReverted;

    auto return_value =
        trait_meta_t::template decode_return<Name>(return_span,
                                                   manually_reverted);
    if (!return_value.has_value()) {
      trace("Invoke Error: Decode Return Failed: {}", return_value.error());
      return std::pair{return_value.error(), res.second};
    }
    return std::pair{return_value.value(), res.second};
  }

private:
  const ContractAddress contract_address_;
};

} // namespace hogd
