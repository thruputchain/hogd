#include "hogd/hog_auth/status.h"

#include <string>

#include "auth_rpc.hh"

namespace hogd {

std::string
AuthErrorCategory::message(int code) const
{
  switch (static_cast<AuthErrorCode>(code)) {
  case AuthErrorCode::kAuthSuccess:
    return "kAuthSuccess";
  case AuthErrorCode::kAuthFailed:
    return "kAuthFailed";
  case AuthErrorCode::kAuthStaleBlockHeight:
    return "kAuthStaleBlockHeight";
  case AuthErrorCode::kAuthInternal:
    return "kAuthInternal";
    break;
  }
  return "Unknown auth error";
}

} // namespace hogd
