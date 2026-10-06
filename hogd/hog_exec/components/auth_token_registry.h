#pragma once

#include <optional>
#include <unordered_map>

#include "auth_token.hh"
#include "hogd/base/rc.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/components/auth_subtoken.h"
#include "types.hh"

namespace hogd {

// A minted auth token, or a subtoken unwrapped from one: the token's mint
// address and expiry height, and the node, shared with the token it came from.
struct RegisteredAuthToken {
  ContractAddress mint_address;
  BlockHeight expiry_height;
  Rc<AuthSubtoken> node;
};

struct AuthTokenUnwrapResult {
  uint32_t new_token_id_start;
  uint32_t new_token_id_count;
};

class AuthTokenRegistry {
public:
  std::optional<uint32_t>
  register_auth_token(ContractAddress const &mint_address,
                      BlockHeight expiry_height, Rc<AuthSubtoken> root);
  RegisteredAuthToken const *lookup_auth_token(uint32_t token_id) const;

  std::optional<AuthTokenUnwrapResult> unwrap_auth_token(uint32_t token_id);

private:
  struct AuthTokenEntry {
    RegisteredAuthToken token;
    std::optional<AuthTokenUnwrapResult> unwrap_result;
  };

  std::unordered_map<uint32_t, AuthTokenEntry> auth_tokens_;
  uint32_t next_token_id_ = 0;
};

} // namespace hogd
