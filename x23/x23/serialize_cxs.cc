
#include "x23/serialize_cxs.h"

namespace x23 {

std::string
CxsErrorCategory::message(int code) const
{
  using enum CxsErrorCode;
  switch (static_cast<CxsErrorCode>(code)) {
  case kTruncated:
    return "truncated input";
  case kNonCanonical:
    return "non-canonical representation";
  case kBadDiscriminant:
    return "bad union discriminant";
  case kLengthExceeded:
    return "length exceeds maximum";
  case kRecursionLimit:
    return "recursion limit exceeded";
  case kTrailingData:
    return "trailing data after value";
  default:
    return "unknown cxs error";
  }
}

} // namespace x23
