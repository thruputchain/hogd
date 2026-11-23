#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "event.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/utility.h"
#include "x23/x23.h"

namespace hogd {

class NativeSyscallContext;

// Stage an unchecked native-token debit of `from` and credit of `to`: no
// balance check, no Transfer event, so callers on the public ERC20 surface
// must run the strict checks first. `amount` must be nonnegative; both writes
// are charged to `ctx`.
[[nodiscard]] x23::Async<InvokeStatus<void>>
native_token_unchecked_transfer(NativeSyscallContext &ctx,
                                ContractAddress const &from,
                                ContractAddress const &to, int128_t amount);

std::unique_ptr<NativeContract> make_native_token_contract();

// Event-signature topics (topic 0) for the native token's standard asset
// events. These reproduce the SDK's event-signature scheme,
// sha256("<major>::<crate>::<EventName>"), for the `thruput-erc20-if` crate
// (currently version 0.1.x, so the prefix is "0.1"), so that the native token
// emits events byte-identical to the canonical RV32 `itc_erc20` contract.
Topic const &native_token_transfer_event_signature();
Topic const &native_token_allowance_change_event_signature();
Topic const &native_token_mint_event_signature();
Topic const &native_token_allowance_delete_event_signature();

// Event bodies not covered by the shared asset-event schema. Leading addresses
// become topics; the remaining fields are the CXS payload.
struct NativeTokenAllowanceChangeEvent {
  ContractAddress owner_address;
  ContractAddress spender_address;
  int128_xdr difference;
};

struct NativeTokenMintEvent {
  ContractAddress to_address;
  int128_xdr amount;

  bool operator==(const NativeTokenMintEvent &) const noexcept = default;
};

struct NativeTokenAllowanceDeleteEvent {
  ContractAddress owner_address;
  ContractAddress spender_address;
  int128_xdr removed;

  bool
  operator==(const NativeTokenAllowanceDeleteEvent &) const noexcept = default;
};

template<size_t I, x23::references<NativeTokenAllowanceChangeEvent> T>
requires (I < 3)
constexpr decltype(auto)
get(T &&t)
{
  if constexpr (I == 0)
    return (std::forward<T>(t).owner_address);
  if constexpr (I == 1)
    return (std::forward<T>(t).spender_address);
  if constexpr (I == 2)
    return (std::forward<T>(t).difference);
}

template<size_t I, x23::references<NativeTokenMintEvent> T> requires (I < 2)
constexpr decltype(auto)
get(T &&t)
{
  if constexpr (I == 0)
    return (std::forward<T>(t).to_address);
  if constexpr (I == 1)
    return (std::forward<T>(t).amount);
}

template<size_t I, x23::references<NativeTokenAllowanceDeleteEvent> T>
requires (I < 3)
constexpr decltype(auto)
get(T &&t)
{
  if constexpr (I == 0)
    return (std::forward<T>(t).owner_address);
  if constexpr (I == 1)
    return (std::forward<T>(t).spender_address);
  if constexpr (I == 2)
    return (std::forward<T>(t).removed);
}

} // namespace hogd

// XDR metadata for the event bodies above.
template<>
struct std::tuple_size<hogd::NativeTokenAllowanceChangeEvent>
  : std::integral_constant<std::size_t, 3> {};
template<std::size_t I> requires (I < 3)
struct std::tuple_element<I, hogd::NativeTokenAllowanceChangeEvent> {
  using struct_type = hogd::NativeTokenAllowanceChangeEvent;
  using type = decltype(auto(get<I>(struct_type{})));
};

template<>
struct std::tuple_size<hogd::NativeTokenMintEvent>
  : std::integral_constant<std::size_t, 2> {};
template<std::size_t I> requires (I < 2)
struct std::tuple_element<I, hogd::NativeTokenMintEvent> {
  using struct_type = hogd::NativeTokenMintEvent;
  using type = decltype(auto(get<I>(struct_type{})));
};

template<>
struct std::tuple_size<hogd::NativeTokenAllowanceDeleteEvent>
  : std::integral_constant<std::size_t, 3> {};
template<std::size_t I> requires (I < 3)
struct std::tuple_element<I, hogd::NativeTokenAllowanceDeleteEvent> {
  using struct_type = hogd::NativeTokenAllowanceDeleteEvent;
  using type = decltype(auto(get<I>(struct_type{})));
};

// NOLINTBEGIN(readability-identifier-naming)
template<> struct x23::xdr_meta<hogd::NativeTokenAllowanceChangeEvent> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "NativeTokenAllowanceChangeEvent";
  static constexpr std::array<std::string_view, 3> field_names{
      "owner_address",
      "spender_address",
      "difference",
  };
};

template<> struct x23::xdr_meta<hogd::NativeTokenMintEvent> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "NativeTokenMintEvent";
  static constexpr std::array<std::string_view, 2> field_names{
      "to_address",
      "amount",
  };
};

template<> struct x23::xdr_meta<hogd::NativeTokenAllowanceDeleteEvent> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "NativeTokenAllowanceDeleteEvent";
  static constexpr std::array<std::string_view, 3> field_names{
      "owner_address",
      "spender_address",
      "removed",
  };
};
// NOLINTEND(readability-identifier-naming)
