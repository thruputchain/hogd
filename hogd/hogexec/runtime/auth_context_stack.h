#pragma once

#include <cstdint>
#include <vector>

#include "auth.hh"

namespace hogd {

struct AuthContextStack {
  std::vector<AuthContext> context_;
  // elements in context_ strictly below fixed_context_height
  // cannot be popped from the stack in the current call frame
  const uint32_t fixed_context_height_;

  AuthContextStack() : fixed_context_height_(0) {}

  explicit AuthContextStack(AuthContextStack const &base)
    : context_(base.context_), fixed_context_height_(base.context_.size())
  {}

  bool can_pop_context() const
  {
    return context_.size() > fixed_context_height_;
  }
};

} // namespace hogd
