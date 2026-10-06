#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>

#include "auth_token.hh"
#include "hogd/base/rc.h"
#include "hogd/base/types.h"

namespace hogd {

// One node of a minted auth token. Immutable once built, so unwrapping a node
// shares its children rather than copying them.
struct AuthSubtoken {
  ContractAddress mint_address; // the token's; the same in every node
  BlockHeight expiry_height;    // likewise
  ContractAddress consumer_address;
  AuthTokenPayload payload;
  // The first child_count slots hold the node's children.
  std::array<Rc<const AuthSubtoken>, kAuthTokenMaxChildNodes> child_slots;
  size_t child_count = 0;

  std::span<Rc<const AuthSubtoken> const> children() const
  {
    return std::span(child_slots).first(child_count);
  }
};

struct AuthTokenUnwrapResult {
  uint32_t new_token_id_start;
  uint32_t new_token_id_count;
};

class AuthTokenRegistry {
public:
  std::optional<uint32_t> register_auth_token(AuthToken auth_token);
  AuthSubtoken const *lookup_auth_token(uint32_t token_id) const;

  std::optional<AuthTokenUnwrapResult> unwrap_auth_token(uint32_t token_id);

private:
  struct AuthTokenEntry {
    Rc<const AuthSubtoken> subtoken;
    std::optional<AuthTokenUnwrapResult> unwrap_result;
  };

  std::unordered_map<uint32_t, AuthTokenEntry> auth_tokens_;
  uint32_t next_token_id_ = 0;
};

} // namespace hogd
