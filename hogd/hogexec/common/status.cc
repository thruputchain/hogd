#include "hogd/hogexec/common/status.h"

#include <cassert>
#include <string>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "syscall_error.hh"
#include "transaction.hh"
#include "x23/format.h"

namespace hogd {

std::string
SystemErrorCategory::message(int code) const
{
  assert(SystemError::is_in_enum_range(code));
  return x23::show(static_cast<SystemErrorCode>(code));
}

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

std::pair<SystemStatus<InvocationFrameStatusCode>, uint64_t>
InvokeResult::to_frame_status() const
{
  if (!result_.has_value()) {
    using enum InvokeErrorCode;

    switch (result_.error().code()) {
    case Unrecoverable:
      return {SystemError{SystemErrorCode::Unrecoverable}, gas_consumed_};
    case Deterministic:
      // i.e. trap within Wasmi
    case OutOfGas:
      // child frame ran out of the gas that was allocated to it
      // Note that that does not mean that the entire transaction is out of gas.
      // The amount of gas consumed will be checked against the parent's gas
      // limit in the syscall_entry
      return {InvocationFrameStatusCode::Fault, gas_consumed_};
    case Abandon:
      return {SystemError{SystemErrorCode::Abandon}, gas_consumed_};
    case ValidationFailure:
      return {SystemError{SystemErrorCode::ValidationFailure}, gas_consumed_};
    default:
      error("Unrecoverable: InvokeResult::to_frame_status got unknown "
            "InvokeErrorCode {}",
            std::to_underlying(result_.error().code()));
      return {SystemError{SystemErrorCode::Unrecoverable}, gas_consumed_};
    }
    // unreachable
    assert(false);
  }

  return {InvocationFrameStatusCode::Return, gas_consumed_};
}

} // namespace hogd
