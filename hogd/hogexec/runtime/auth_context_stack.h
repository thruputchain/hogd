#pragma once

#include <cstdint>
#include <vector>

#include "auth.hh"

namespace hogd {

struct AuthContextStack {
  std::vector<AuthContext> context_;
  // elements in context_ strictly below fixed_context_height
  // cannot be popped from the stack in the current call frame
  uint32_t fixed_context_height_;

  bool can_pop_context() const {
    return context_.size() > fixed_context_height_;
  }
};



} // namespace hogd
