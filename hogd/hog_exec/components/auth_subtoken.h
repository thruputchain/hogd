#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <utility>

#include "auth_token.hh"
#include "hogd/base/rc.h"
#include "hogd/base/rc_xdr.h"
#include "types.hh"
#include "x23/utility.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

// AuthTokenNode, with each child held by an Rc, so that unwrapping a node
// shares its children rather than copying them. It has AuthTokenNode's wire
// format, and x23 decodes a serialized AuthTokenNode straight into a tree of
// these.
struct AuthSubtoken {
  ContractAddress consumer_address;
  AuthTokenPayload payload;
  x23::Vec<Rc<AuthSubtoken>, kAuthTokenMaxChildNodes> child_nodes;
};

// AuthToken, with its root node held by an Rc. It has AuthToken's wire format.
// A subtoken unwrapped from a token is registered as the token's mint address
// and expiry height with that subtoken as the root.
struct RegisteredAuthToken {
  ContractAddress mint_address;
  BlockHeight expiry_height;
  Rc<AuthSubtoken> root_node;
};

} // namespace hogd

//
// XDR internals, as x23c generates them for AuthTokenNode and AuthToken.
//

namespace hogd {
template<size_t I, x23::references<AuthSubtoken> T> requires (I < 3)
constexpr decltype(auto)
get(T &&t)
{
  if constexpr (I == 0)
    return (std::forward<T>(t).consumer_address);
  if constexpr (I == 1)
    return (std::forward<T>(t).payload);
  if constexpr (I == 2)
    return (std::forward<T>(t).child_nodes);
}
} // namespace hogd
template<>
struct std::tuple_size<hogd::AuthSubtoken>
  : std::integral_constant<std::size_t, 3> {};
template<std::size_t I> requires (I < 3)
struct std::tuple_element<I, hogd::AuthSubtoken> {
  using type = decltype(auto(get<I>(hogd::AuthSubtoken{})));
};
// NOLINTBEGIN(readability-identifier-naming)
template<> struct x23::xdr_meta<hogd::AuthSubtoken> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "AuthTokenNode";
  static constexpr std::array<std::string_view, 3> field_names{
      "consumer_address",
      "payload",
      "child_nodes",
  };
};
// NOLINTEND(readability-identifier-naming)

namespace hogd {
template<size_t I, x23::references<RegisteredAuthToken> T> requires (I < 3)
constexpr decltype(auto)
get(T &&t)
{
  if constexpr (I == 0)
    return (std::forward<T>(t).mint_address);
  if constexpr (I == 1)
    return (std::forward<T>(t).expiry_height);
  if constexpr (I == 2)
    return (std::forward<T>(t).root_node);
}
} // namespace hogd
template<>
struct std::tuple_size<hogd::RegisteredAuthToken>
  : std::integral_constant<std::size_t, 3> {};
template<std::size_t I> requires (I < 3)
struct std::tuple_element<I, hogd::RegisteredAuthToken> {
  using type = decltype(auto(get<I>(hogd::RegisteredAuthToken{})));
};
// NOLINTBEGIN(readability-identifier-naming)
template<> struct x23::xdr_meta<hogd::RegisteredAuthToken> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "AuthToken";
  static constexpr std::array<std::string_view, 3> field_names{
      "mint_address",
      "expiry_height",
      "root_node",
  };
};
// NOLINTEND(readability-identifier-naming)
