#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
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

class SystemCallHandler;

// Stage an unchecked native-token debit of `from` and credit of `to`.
// No balance check, no Transfer event: callers on the public ERC20 surface
// must run the strict checks first; the genesis initial-token-claim calls
// this directly because its in-tx balance is transiently short by the
// storage deposit that hog_db's exclusive-delete refund nets at commit.
// `amount` must be nonnegative.
x23::Async<InvokeStatus<void>>
native_token_unchecked_transfer(SystemCallHandler &handler,
                                ContractAddress const &from,
                                ContractAddress const &to, int128_t amount);

std::unique_ptr<NativeContract> make_native_token_contract();

// Event-signature topics (topic 0) for the native token's standard asset
// events. These reproduce the SDK's event-signature scheme,
// sha256("<major>::<crate>::<EventName>"), for the `groundhog-erc20-if` crate
// (currently version 0.1.x, so the prefix is "0.1"), so that the native token
// emits events byte-identical to the canonical RV32 `itc_erc20` contract.
Topic const &native_token_transfer_event_signature();
Topic const &native_token_approval_event_signature();
Topic const &native_token_mint_event_signature();
Topic const &native_token_burn_event_signature();

// Bodies of the native token's mint and burn events, in the layout
// `make_event_from_cxs_struct` expects: the leading `ContractAddress` becomes
// topic 1 and the amount becomes the CXS-encoded payload, so each of these
// events carries two topics where a Transfer carries three.
//
// `groundhog-erc20-if` gives a supply change its own event instead of
// overloading Transfer with the zero address, and keeps the two bodies in the
// interface crate rather than in the shared asset-event schema --- on
// Groundhog the zero address is an address like any other, so a transfer
// involving it is exactly that. There is therefore no `standard_asset_events`
// counterpart to reuse, and hogd mirrors the SDK's shape here.
struct NativeTokenMintEvent {
  ContractAddress to_address;
  int128_xdr amount;

  bool operator==(const NativeTokenMintEvent &) const noexcept = default;
};

struct NativeTokenBurnEvent {
  ContractAddress from_address;
  int128_xdr amount;

  bool operator==(const NativeTokenBurnEvent &) const noexcept = default;
};

template<size_t I, x23::references<NativeTokenMintEvent> T> requires (I < 2)
constexpr decltype(auto)
get(T &&t)
{
  if constexpr (I == 0)
    return (std::forward<T>(t).to_address);
  if constexpr (I == 1)
    return (std::forward<T>(t).amount);
}

template<size_t I, x23::references<NativeTokenBurnEvent> T> requires (I < 2)
constexpr decltype(auto)
get(T &&t)
{
  if constexpr (I == 0)
    return (std::forward<T>(t).from_address);
  if constexpr (I == 1)
    return (std::forward<T>(t).amount);
}

} // namespace hogd

// Hand-written stand-ins for what x23c emits for a `.x` struct, so the two
// event bodies above satisfy `x23::is_xdr_struct` and serialize field by
// field.
template<>
struct std::tuple_size<hogd::NativeTokenMintEvent>
  : std::integral_constant<std::size_t, 2> {};
template<std::size_t I> requires (I < 2)
struct std::tuple_element<I, hogd::NativeTokenMintEvent> {
  using struct_type = hogd::NativeTokenMintEvent;
  using type = decltype(auto(get<I>(struct_type{})));
};

template<>
struct std::tuple_size<hogd::NativeTokenBurnEvent>
  : std::integral_constant<std::size_t, 2> {};
template<std::size_t I> requires (I < 2)
struct std::tuple_element<I, hogd::NativeTokenBurnEvent> {
  using struct_type = hogd::NativeTokenBurnEvent;
  using type = decltype(auto(get<I>(struct_type{})));
};

// NOLINTBEGIN(readability-identifier-naming)
template<> struct x23::xdr_meta<hogd::NativeTokenMintEvent> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "NativeTokenMintEvent";
  static constexpr std::array<std::string_view, 2> field_names{
      "to_address",
      "amount",
  };
};

template<> struct x23::xdr_meta<hogd::NativeTokenBurnEvent> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "NativeTokenBurnEvent";
  static constexpr std::array<std::string_view, 2> field_names{
      "from_address",
      "amount",
  };
};
// NOLINTEND(readability-identifier-naming)
