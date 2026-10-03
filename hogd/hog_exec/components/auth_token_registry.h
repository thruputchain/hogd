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
  // True iff every root-to-leaf path has at most max_depth nodes.
  static bool node_within_depth(AuthTokenNode const &root, uint32_t max_depth);

  // The tree's node count when within max_depth and at most max_nodes nodes;
  // nullopt otherwise. The walk stops at the first breach, bounding its cost.
  static std::optional<uint32_t>
  count_nodes_within_depth(AuthTokenNode const &root, uint32_t max_depth,
                           uint32_t max_nodes);

  std::optional<uint32_t> register_auth_token(AuthToken auth_token);
  AuthToken const *lookup_auth_token(uint32_t token_id) const;

  std::optional<AuthTokenUnwrapResult> unwrap_auth_token(uint32_t token_id);

private:
  struct AuthTokenEntry {
    AuthToken auth_token;
    std::optional<AuthTokenUnwrapResult> unwrap_result;
  };

  std::unordered_map<uint32_t, AuthTokenEntry> auth_tokens_;
  uint32_t next_token_id_ = 0;
};

} // namespace hogd
