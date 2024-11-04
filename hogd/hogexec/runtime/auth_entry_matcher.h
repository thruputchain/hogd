#pragma once

#include <vector>

#include "auth.hh"
#include "hogd/hogexec/runtime/auth_context.h"

namespace hogd {

class AuthEntryMatcher {
public:
  explicit AuthEntryMatcher(AuthEntry const &entry);
  bool try_match(AuthContextStack const &auth_context_stack);

private:
  struct AuthEntryNodeFrame {
    AuthContext context_;
    std::vector<AuthEntryNodeFrame> children_;
    bool consumed_ = false;
    const bool is_leaf_;

    explicit AuthEntryNodeFrame(AuthEntryNode const &node);
  };

  AuthEntryMetadata auth_metadata_;
  std::vector<AuthEntryNodeFrame> nodes_;
};

} // namespace hogd
