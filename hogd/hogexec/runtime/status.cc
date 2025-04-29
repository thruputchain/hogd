#include "hogd/hogexec/runtime/status.h"

#include <cassert>
#include <string>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
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
      return {SyscallErrorCode::kDeterministicWasm, gas_consumed_};
    case kInvokeOutOfGas:
      return {HostFnError{HostFnErrorCode::kHostOutOfGas}, gas_consumed_};
    default:
      return {HostFnError{HostFnErrorCode::kHostUnrecoverable}, gas_consumed_};
    }
    // unreachable
    assert(false);
  }

  // This converts the user-defined return value to a syscall error code.
  // We could do a check here (i.e. reserve < 2^16 for system, everything else
  // for users) but this doesn't particularly matter for correctness.
  return {static_cast<SyscallErrorCode>(*result_), gas_consumed_};
}

InvokeResult
InvokeResult::to_root_error() const
{
  assert(!is_root_success());

  if (result_) {
    return InvokeResult{.result_ =
                            InvokeError{InvokeErrorCode::kInvokeDeterministic},
                        .gas_consumed_ = gas_consumed_};
  }
  return *this;
}

} // namespace hogd
