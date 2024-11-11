#include "hogd/hogexec/runtime/status.h"

#include <cassert>
#include <string>

#include "syscall.hh"
#include "x23/format.h"

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
  case kUnrecoverable:
    return "kUnrecoverable";
  case kReturn:
    return "kReturn";
  case kExit:
    return "kExit";
  case kRevert:
    return "kRevert";
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
  case kDeterministic:
    return "kDeterministic";
  case kOutOfGas:
    return "kOutOfGas";
  case kUnrecoverable:
    return "kUnrecoverable";
  }
  return "Unknown";
}

std::string
SyscallErrorCategory::message(int code) const
{
  assert(SyscallError::is_in_enum_range(code));
  return x23::show(static_cast<SyscallErrorCode>(code));
}

} // namespace hogd
