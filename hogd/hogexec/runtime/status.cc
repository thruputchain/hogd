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

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
InvokeResult::to_host_fn_result() const
{
  if (!result_.has_value()) {
    using enum InvokeErrorCode;

    switch (result_.error().code()) {
    case kUnrecoverable:
      return {HostFnError{HostFnErrorCode::kUnrecoverable}, gas_consumed_};
    case kDeterministic:
      // i.e. trap within Wasmi
      return {SyscallErrorCode::kDeterministicWasm, gas_consumed_};
    case kOutOfGas:
      return {HostFnError{HostFnErrorCode::kOutOfGas}, gas_consumed_};
    default:
      return {HostFnError{HostFnErrorCode::kUnrecoverable}, gas_consumed_};
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
    return InvokeResult{.result_ = InvokeError{InvokeErrorCode::kDeterministic},
                        .gas_consumed_ = gas_consumed_};
  }
  return *this;
}

} // namespace hogd
