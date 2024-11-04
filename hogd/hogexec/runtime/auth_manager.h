#pragma once

#include <map>

#include "auth.hh"
#include "transaction.hh"
#include "types.hh"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/auth_entry_matcher.h"

namespace hogd {

class AuthManager {
public:
  AuthManager(SignedTransaction const& stx);

  bool try_match(ContractAddress const &address,
                 AuthContextStack const &auth_context_stack);
  
  struct AuthEntryContext {
    AuthEntryMatcher matcher_;
    bool auth_checked_ = false;
  };

  std::map<ContractAddress, AuthEntryContext> auth_entries_;

  static bool initialization_validate_auth_entries(SignedTransaction const& stx);
};

} // namespace hogd
