#include <cassert>
#include <string>

#include "hogd/hogexec/runtime/status.h"
#include "syscall.hh"

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
  case kReturn:
    return "kReturn";
  default:
    return "HostFnErrorCode Unknown";
  }
}

std::string
InvokeErrorCategory::message(int code) const
{
  using enum InvokeErrorCode;
  assert(InvokeError::is_in_enum_range(code));
  switch (static_cast<InvokeErrorCode>(code)) {
  case kNone:
    return "kNone";
  case kDeterministicError:
    return "kDeterministicError";
  case kOutOfGasError:
    return "kOutOfGasError";
  case kUnrecoverableSystemError:
    return "kUnrecoverableSystemError";
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
