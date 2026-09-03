#pragma once

#include <optional>
#include <unordered_map>

#include "auth_token.hh"
#include "hogd/base/types.h"

namespace hogd {

struct AuthTokenUnwrapResult {
  uint32_t new_token_id_start;
  uint32_t new_token_id_count;
};

class AuthTokenRegistry {
public:
  static bool node_within_depth(AuthTokenNode const &root, uint32_t max_depth);

  std::optional<uint32_t> register_auth_token(AuthToken auth_token);
  AuthToken const *lookup_auth_token(uint32_t token_id) const;

  std::optional<AuthTokenUnwrapResult> unwrap_auth_token(uint32_t token_id);

  // Counts one mint attempt against the per-transaction cap and reports
  // whether it is allowed: false once kMaxAuthTokensPerTransaction attempts
  // have been made. Callers invoke this before deserializing the untrusted
  // token so the cap bounds decode/traversal work whether or not a given tree
  // parses -- not just the successful mints.
  bool try_begin_mint_attempt();

private:
  struct AuthTokenEntry {
    AuthToken auth_token;
    std::optional<AuthTokenUnwrapResult> unwrap_result;
  };

  std::unordered_map<uint32_t, AuthTokenEntry> auth_tokens_;
  uint32_t next_token_id_ = 0;
  // Counts mint attempts (successful or not); unwrapping a token into its
  // children is not a mint and does not add to it.
  uint32_t mint_attempts_ = 0;
};

} // namespace hogd
