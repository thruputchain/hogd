#include "hogd/hogexec/runtime/status.h"

#include <cassert>
#include <string>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "syscall_error.hh"
#include "x23/format.h"

namespace hogd {

std::string
SystemErrorCategory::message(int code) const
{
  assert(SystemError::is_in_enum_range(code));
  switch (static_cast<SystemErrorCode>(code)) {
  case SystemErrorCode::kUnrecoverable:
    return "kUnrecoverable";
  case SystemErrorCode::kAbortTransaction:
    return "kAbortTransaction";
  case SystemErrorCode::kValidationFailure:
    return "kValidationFailure";
  default:
    assert(false);
    std::unreachable();
  }
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

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
InvokeResult::to_host_fn_result() const
{
  if (!result_.has_value()) {
    using enum InvokeErrorCode;

    switch (result_.error().code()) {
    case kInvokeUnrecoverable:
      return {HostFnError{HostFnErrorCode::kHostUnrecoverable}, gas_consumed_};
    case kInvokeDeterministic:
      // i.e. trap within Wasmi
    case kInvokeOutOfGas:
      // child frame ran out of the gas that was allocated to it
      // Note that that does not mean that the entire transaction is out of gas.
      // The amount of gas consumed will be checked against the parent's gas
      // limit in the syscall_entry
      return {SyscallErrorCode::kSyscallErrorCodeDeterministicWasm,
              gas_consumed_};
    case kInvokeAbortTransaction:
      return {HostFnError{HostFnErrorCode::kHostAbortTransaction},
              gas_consumed_};
    case kInvokeValidationFailure:
      return {HostFnError{HostFnErrorCode::kHostValidationFailure},
              gas_consumed_};
    default:
      return {HostFnError{HostFnErrorCode::kHostUnrecoverable}, gas_consumed_};
    }
    // unreachable
    assert(false);
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, gas_consumed_};
}

} // namespace hogd
