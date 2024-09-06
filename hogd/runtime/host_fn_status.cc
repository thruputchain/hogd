#include "hogd/runtime/host_fn_status.h"

#include <string>

namespace hogd {

std::string
HostFnErrorCategory::message(int code) const
{
  using enum HostFnErrorCode;
  switch (static_cast<HostFnErrorCode>(code)) {
  case kUnrecoverableUnknown:
    return "UNRECOVERABLE_UNKNOWN";
  case kRecoverableUnknown:
    return "RECOVERABLE_UNKNOWN";
  default:
    return "";
  }
}

bool
HostFnError::recoverable() const
{
  using enum HostFnErrorCode;
  switch (static_cast<HostFnErrorCode>(code())) {
  case kUnrecoverableUnknown:
    return false;
  case kRecoverableUnknown:
    return true;
  default:
    return false;
  }
}

} // namespace hogd
