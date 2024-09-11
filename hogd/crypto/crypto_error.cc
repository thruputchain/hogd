#include "hogd/crypto/crypto_error.h"

#include <limits>
#include <string>
#include <type_traits>

namespace hogd {
std::string
CryptoErrorCategory::message(int code) const
{
  using enum CryptoErrorCode;

  // UB to convert code to CryptoErrorCode unless
  // the int fits within the enum
  // In theory this could be called with an int not from a CryptoError,
  // although in practice/normal usage that won't happen.
  using enum_backing_t = std::underlying_type_t<CryptoErrorCode>;

  if (code < std::numeric_limits<enum_backing_t>::min() ||
      code > std::numeric_limits<enum_backing_t>::max()) {
    return "unknown out of range";
  }

  switch (static_cast<CryptoErrorCode>(code)) {
  case kSiphash24Error:
    return "kSiphash24Error";
  case kSHA256Error:
    return "kSha256Error";
  case kInitializeFailure:
    return "kInitializeFailure";
  }
  return "unknown";
}

} // namespace hogd
