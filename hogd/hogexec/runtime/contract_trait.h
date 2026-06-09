#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "built_contracts/contract_loader.h"
#include "contract.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/components/contract_result.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "serialize_calldata.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/debug.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

// Common utilities for building preset contracts

ContractEntrypointID
sdk_entrypoint_derivation(std::string_view identifier_name,
                          std::string_view identifier_version,
                          std::string_view method_name);
Runtime::MethodID sdk_method_id(ContractEntrypointID entry_point_id);

// `T` is a compile-time tag. XDR/TXS/CXS: 0 bytes (same as void / empty struct
// / `x23::span<std::uint8_t, 0>`). `std::tuple_size` + 0-field `xdr_meta` make
// `std::tuple<Signer, ...>` a valid `x23::is_xdr_type` struct for calldata.
//
// `token_id` is C++ / host state only; it is not an XDR field (never
// marshaled).
template<typename T> struct Signer {

  uint32_t token_id_{};
};

} // namespace hogd

namespace std {
template<typename T>
struct tuple_size<hogd::Signer<T>> : std::integral_constant<std::size_t, 0> {};
} // namespace std

namespace x23 {
// Same wire format as XDR void; see `serialize_equiv<void, …>` in x23.h.
template<typename T>
struct serialize_equiv<void, hogd::Signer<T>> : std::true_type {};
template<typename T>
struct serialize_equiv<span<std::uint8_t, 0>, hogd::Signer<T>>
  : std::true_type {};

// NOLINTBEGIN(readability-identifier-naming)
template<typename T> struct xdr_meta<hogd::Signer<T>> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "Signer";
  static constexpr std::array<std::string_view, 0> field_names{};
};
// NOLINTEND(readability-identifier-naming)
} // namespace x23

namespace hogd {

// Get the method ID for a given method,
// at which contracts using the SDK will expect to find it.
inline Runtime::MethodID
sdk_method_id(std::string_view identifier_name,
              std::string_view identifier_version, std::string_view method_name)
{
  return sdk_method_id(sdk_entrypoint_derivation(identifier_name,
                                                 identifier_version,
                                                 method_name));
}
ContractEntrypointID entrypoint_from_method_id(Runtime::MethodID method_id);

namespace detail {

template<typename> struct CalldataArgs;

template<typename R, typename... Args> struct CalldataArgs<R (*&&)(Args...)> {
  using args_t = std::tuple<Args...>;
  using return_t = std::conditional_t<std::is_same_v<R, void>, std::tuple<>, R>;
};

constexpr static std::array<std::pair<std::string_view, SerializationFormat>, 2>
    kSerializationFormatLookup = {
        std::pair{"txs", SerializationFormat::kTxs},
        std::pair{"cxs", SerializationFormat::kCxs},
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
  constexpr static size_t kLookupIndex = [] {
    for (size_t i = 0; i < ContractTrait::kExportNames.size(); ++i) {
      if (ContractTrait::kExportNames[i] == Name.get()) {
        return i;
      }
    }
    throw "ContractTraitMeta: Method name not found";
  }();

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

  constexpr static auto load_wasm() requires requires {
    { ContractTrait::kWasmFile } -> std::convertible_to<std::string_view>;
  }
  {
    return load_contract(ContractTrait::kWasmFile);
  }

  // Loads the contract's compiled RV32 artifact (<name>.rv32a, derived from
  // kWasmFile) as a kRv32 contract. The artifact filename mirrors the wasm one
  // produced by the build, so no separate trait field is needed.
  static std::shared_ptr<Contract> load_rv32() requires requires {
    { ContractTrait::kWasmFile } -> std::convertible_to<std::string_view>;
  }
  {
    constexpr std::string_view kWasmSuffix = ".wasm";
    std::string_view wasm_file = ContractTrait::kWasmFile;
    // kWasmFile must end in .wasm to derive the matching .rv32a name.
    x23assert(wasm_file.ends_with(kWasmSuffix));
    std::string rv32_file(
        wasm_file.substr(0, wasm_file.size() - kWasmSuffix.size()));
    rv32_file += ".rv32a";

    auto contract = std::make_shared<Contract>(ContractType::kRv32);
    contract->rv32_contract() = load_contract_blob(rv32_file);
    return contract;
  }

  template<StringLiteral Name> static ContractEntrypointID entrypoint()
  {
    auto entrypoint =
        sdk_entrypoint_derivation(ContractTrait::kTraitName,
                                  ContractTrait::kVersion, Name.get());
    if constexpr (requires { ContractTrait::kEntrypoints; }) {
      x23assert(ContractTrait::kEntrypoints[kLookupIndex<Name>] ==
                sdk_method_id(entrypoint));
    }
    return entrypoint;
  }

  template<StringLiteral Name, typename... Args>
  static Calldata make_calldata(Args const &...args) requires (
      std::same_as<
          std::tuple<Args...>,
          typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::args_t>)
  {
    using args_t =
        typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::args_t;
    return make_encoding(args_t{args...});
  }

  template<StringLiteral Name, typename ReturnType>
  static Calldata make_return(ReturnType return_value) requires (
      std::same_as<ReturnType, typename detail::CalldataArgs<
                                   signature<kLookupIndex<Name>>>::return_t>)
  {
    return make_encoding(return_value);
  }

  // Returns nullopt if decoding fails.
  // Currently only used via invoke_opt (tests).
  // Error messages are at error level to make test failures clear.
  template<StringLiteral Name>
  static auto decode_return(std::span<const std::byte> data,
                            bool manually_reverted)
  {
    using return_type =
        typename detail::CalldataArgs<signature<kLookupIndex<Name>>>::return_t;
    return [data, manually_reverted] -> std::optional<return_type> {
      if constexpr (is_contract_result_type<return_type>) {
        if (manually_reverted) {
          // New format: abort buffer contains CXS-encoded (u64 selector, Buf
          // body). ContractErrorType parses selector+body and dispatches
          // internally.
          ReturnBuffer abort_buf(
              reinterpret_cast<const uint8_t *>(data.data()),
              reinterpret_cast<const uint8_t *>(data.data()) + data.size());
          return return_type::make_err(std::move(abort_buf));
        }
        // ok type
        auto out = Serializer<kSerializationFormat>::template deserialize<
            typename return_type::ok_type>(data);
        if (!out.has_value()) {
          error("Decode Return Failed: Failed to deserialize return ok type");
          return std::nullopt;
        }
        return return_type::make_ok(*out);
      }
      else {
        // not using the manual revert path
        if (manually_reverted) {
          error("Decode Return Failed: Manually reverted in non-result return "
                "type");
          return std::nullopt;
        }
        auto out =
            Serializer<kSerializationFormat>::template deserialize<return_type>(
                data);
        if (!out.has_value()) {
          error("Decode Return Failed: Failed to deserialize return value");
          return std::nullopt;
        }
        return out;
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

} // namespace hogd
