#include "hogd/hogexec/runtime/status.h"

#include <cassert>
#include <string>

#include "ffi_enums.hh"
#include "syscall.hh"
#include "x23/format.h"

namespace hogd {

std::string
HostFnErrorCategory::message(int code) const
{
  assert(HostFnError::is_in_enum_range(code));
  return x23::show(static_cast<HostFnErrorCode>(code));
}

std::string
InvokeErrorCategory::message(int code) const
{
  assert(InvokeError::is_in_enum_range(code));
  return x23::show(static_cast<InvokeErrorCode>(code));
}

std::string
SyscallErrorCategory::message(int code) const
{
  assert(SyscallError::is_in_enum_range(code));
  return x23::show(static_cast<SyscallErrorCode>(code));
}

} // namespace hogd
