#pragma once

namespace hogd {

class Authentication {
public:
  static bool authenticate(const SignedTransaction &tx) { return false; }
};

} // namespace hogd
