#include "hogd/runtime/status.h"

#include <limits>
#include <string>
#include <type_traits>

namespace hogd {

std::string
HostFnErrorCategory::message(int code) const
{
  using enum HostFnErrorCode;

  using enum_backing_t = std::underlying_type_t<HostFnErrorCode>;
  if (code < std::numeric_limits<enum_backing_t>::min() ||
      code > std::numeric_limits<enum_backing_t>::max()) {
    return "HostFnErrorCode OOB";
  }
  // TODO(geoff) check on enum limits

  switch (static_cast<HostFnErrorCode>(code)) {
  case kNoneOrRecoverable:
    return "kNoneOrRecoverable";
  case kOutOfGas:
    return "kOutOfGas";
  case kUnrecoverableUnknown:
    return "kUnrecoverableUnknown";
  default:
    return "HostFnErrorCode Unknown";
  }
}

std::string
WasmiInvokeErrorCategory::message(int code) const
{
  using enum WasmiInvokeErrorCode;
  // TODO(geoff) check on enum limits

  switch (static_cast<WasmiInvokeErrorCode>(code)) {
  case kWasmiNone:
    return "kWasmiNone";
  case kWasmiFnNExist:
    return "kWasmiFnNExist";
  case kWasmiInputError:
    return "kWasmiInputError";
  case kWasmiReturnTypeError:
    return "kWasmiReturnTypeError";
  case kWasmiDeterministicCallError:
    return "kWasmiDeterministicCallError";
  case kWasmiOutOfGasError:
    return "kWasmiOutOfGasError";
  case kWasmiUnrecoverableSystemError:
    return "kWasmiUnrecoverableSystemError";
  default:
    return "WasmiInvokeErrorCode Unknown";
  }
}

} // namespace hogd
