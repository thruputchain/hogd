#pragma once

#include <expected>
#include <string>
#include <system_error>

#include "auth_rpc.hh"
#include "hogd/base/status.h"

namespace hogd {

class AuthErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "auth"; }
  std::string message(int code) const override;
};

struct AuthError : public EnumError<AuthErrorCode, AuthErrorCategory> {
  explicit AuthError(AuthErrorCode err) : EnumError(err) {}
};

template<typename T> using AuthStatus = std::expected<T, AuthError>;

} // namespace hogd
