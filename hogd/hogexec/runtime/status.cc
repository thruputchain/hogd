#include <cassert>
#include <string>

#include "hogd/hogexec/runtime/status.h"

namespace hogd {

std::string
HostFnErrorCategory::message(int code) const
{
  using enum HostFnErrorCode;
  assert(HostFnError::is_in_enum_range(code));
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
  assert(WasmiInvokeError::is_in_enum_range(code));
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

std::string
SyscallErrorCategory::message(int code) const
{
  using enum SyscallErrorCode;
  assert(SyscallError::is_in_enum_range(code));
  switch (static_cast<SyscallErrorCode>(code)) {
    // TODO(geoff) unimpl
  default:
    return "SyscallErrorCode Unknown";
  }
}

} // namespace hogd
