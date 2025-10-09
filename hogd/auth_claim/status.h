#pragma once

#include <expected>
#include <string>
#include <system_error>

#include "hogd/base/status.h"

namespace hogd {

enum class AuthVerifyErrorCode {
  // The transaction is malformed.
  // These transactions should never be flooded over the network.
  // Example: a public key id that is out of bounds.
  kAuthMalformed,

  // Signature verification failed.
  kSignatureVerificationFailed,

  // Mismatch between expectations about the auth data on an account
  // and the actual auth data (i.e. referencing a pk that doesn't exist,
  // or failing to satisfy a gas claim).
  kAuthDataMismatch,
};


class AuthVerifyErrorCategory : public std::error_category {
public:
    const char *name() const noexcept override { return "auth_verify"; }
    std::string message(int code) const override;
};

struct AuthVerifyError : public EnumError<AuthVerifyErrorCode, AuthVerifyErrorCategory> {
    explicit AuthVerifyError(AuthVerifyErrorCode err) : EnumError(err) {}
};

// Enable comparisons between HostFnErrorCode and std::error_code.
inline bool
operator==(const AuthVerifyErrorCode &lhs, const std::error_code &rhs)
{
    return AuthVerifyError{lhs} == rhs;
}

template<typename T> using AuthVerifyStatus = std::expected<T, AuthVerifyError>;

}