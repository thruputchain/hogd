#include "hogd/base/status.h"

#include <cassert>
#include <string>

namespace hogd {

std::string
GCategory::message(int code) const
{
  assert(GError::is_in_enum_range(code));

  using enum GCode;
  switch (static_cast<GCode>(code)) {
  case kCancelled:
    return "CANCELLED";
  case kUnknown:
    return "UNKNOWN";
  case kInvalidArgument:
    return "INVALID_ARGUMENT";
  case kDeadlineExceeded:
    return "DEADLINE_EXCEEDED";
  case kNotFound:
    return "NOT_FOUND";
  case kAlreadyExists:
    return "ALREADY_EXISTS";
  case kPermissionDenied:
    return "PERMISSION_DENIED";
  case kResourceExhausted:
    return "RESOURCE_EXHAUSTED";
  case kFailedPrecondition:
    return "FAILED_PRECONDITION";
  case kAborted:
    return "ABORTED";
  case kOutOfRange:
    return "OUT_OF_RANGE";
  case kUnimplemented:
    return "UNIMPLEMENTED";
  case kInternal:
    return "INTERNAL";
  case kUnavailable:
    return "UNAVAILABLE";
  case kDataLoss:
    return "DATA_LOSS";
  case kUnauthorized:
    return "UNAUTHORIZED";
  default:
    return "";
  }
}

} // namespace hogd
