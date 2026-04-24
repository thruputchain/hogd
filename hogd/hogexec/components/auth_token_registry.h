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
  std::optional<uint32_t> register_auth_token(AuthToken auth_token);
  AuthToken const *lookup_auth_token(uint32_t token_id) const;

  std::optional<AuthTokenUnwrapResult> unwrap_auth_token(uint32_t token_id);

private:
  std::unordered_map<uint32_t, AuthToken> auth_tokens_;
  uint32_t next_token_id_ = 0;
};

} // namespace hogd
