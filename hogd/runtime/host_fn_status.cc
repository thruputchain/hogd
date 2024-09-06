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

HostFnErrorCode
wasmi_to_host_fn_error(WasmiInvokeError wasmi_error)
{
  switch (wasmi_error) {
  case WasmiInvokeError::kWasmiLinkError:
    return HostFnErrorCode::kWasmiLinkError;
  case WasmiInvokeError::kWasmiFnNExist:
    return HostFnErrorCode::kWasmiFnNExist;
  case WasmiInvokeError::kWasmiInputError:
    return HostFnErrorCode::kWasmiInputError;
  case WasmiInvokeError::kWasmiReturnTypeError:
    return HostFnErrorCode::kWasmiReturnTypeError;
  case WasmiInvokeError::kWasmiCallError:
    return HostFnErrorCode::kWasmiCallError;
  case WasmiInvokeError::kWasmiHostError:
    return HostFnErrorCode::kWasmiHostError;
  case WasmiInvokeError::kWasmiUnrecoverableSystemError:
    return HostFnErrorCode::kWasmiUnrecoverableSystemError;
  default:
    return HostFnErrorCode::kUnrecoverableUnknown;
  }
}

} // namespace hogd
