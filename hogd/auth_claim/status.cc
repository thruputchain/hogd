#include "hogd/auth_claim/status.h"

#include <cassert>
#include <string>

#include "auth_claim.hh"
#include "x23/format.h"

namespace hogd {

std::string
AuthVerifyErrorCategory::message(int code) const
{
  assert(AuthVerifyError::is_in_enum_range(code));
  return x23::show(static_cast<AuthVerifyErrorCode>(code));
}

} // namespace hogd
