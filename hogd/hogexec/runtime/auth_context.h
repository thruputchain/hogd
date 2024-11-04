#pragma once

#include <vector>

#include "auth.hh"

namespace hogd {

struct AuthContextStack {
  std::vector<AuthContext> context_;
};

} // namespace hogd
