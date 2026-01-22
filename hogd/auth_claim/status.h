#pragma once

#include <expected>
#include <format>
#include <string>
#include <system_error>
#include <utility>

#include "auth_claim.hh"
#include "hogd/base/status.h"

namespace hogd {

class AuthVerifyErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "auth_verify"; }
  std::string message(int code) const override;
};

struct AuthVerifyError
  : public EnumError<AuthVerifyErrorCode, AuthVerifyErrorCategory> {
  explicit AuthVerifyError(AuthVerifyErrorCode err) : EnumError(err) {}
};

// Enable comparisons between HostFnErrorCode and std::error_code.
inline bool
operator==(const AuthVerifyErrorCode &lhs, const std::error_code &rhs)
{
  return AuthVerifyError{lhs} == rhs;
}

template<typename T> using AuthVerifyStatus = std::expected<T, AuthVerifyError>;

} // namespace hogd

template<> struct std::formatter<hogd::AuthVerifyErrorCode> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const hogd::AuthVerifyErrorCode code, auto &ctx)
  {
    static hogd::AuthVerifyErrorCategory category;
    return format_to(ctx.out(), "{}",
                     category.message(std::to_underlying(code)));
  }
};
