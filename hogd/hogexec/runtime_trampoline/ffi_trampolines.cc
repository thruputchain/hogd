#include "hogd/hogexec/runtime_trampoline/ffi_trampolines.h"

#include <concepts>
#include <cstdint>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"

namespace hogd {

class HostCallContext;

namespace {

TrampolineResult
success(int32_t res)
{
  return TrampolineResult{.result = res, .panic = kTrampolineErrorNONE};
}

TrampolineResult
host_exit(int32_t res)
{
  return TrampolineResult{.result = res, .panic = kTrampolineErrorEXIT};
}

TrampolineResult
error(uint8_t error)
{
  return TrampolineResult{.result = 0, .panic = error};
}

TrampolineResult
handle_result(HostFnResult const &res)
{
  switch (res.first) {
  case HostFnErrorCode::kNoneOrRecoverable:
    return success(res.second);
  case HostFnErrorCode::kOutOfGas:
    return error(kTrampolineErrorOUTOFGAS);
  case HostFnErrorCode::kReturn:
    return error(kTrampolineErrorRETURN);
  case HostFnErrorCode::kExit:
    return host_exit(res.second);
  default:
    return error(kTrampolineErrorUNRECOVERABLE);
  }
}

template<std::same_as<uint64_t>... Args>
TrampolineResult
c_call_internal(void *function_pointer, void *host_call_context,
                Args... args) noexcept
{
  try {
    auto *user_ctx = reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr = reinterpret_cast<HostFnResult (*)(HostCallContext *, Args...)>(
        function_pointer);

    return handle_result((*ptr)(user_ctx, args...));

  } catch (...) { // NOLINT(bugprone-empty-catch)
    return error(kTrampolineErrorUNRECOVERABLE);
  }
}

} // namespace

} // namespace hogd

extern "C" {

// These functions catch all errors, just in case our code throws an exception.
// It is UB to pass errors across FFI boundaries, so they're all declared
// noexcept.
// These do not have the noexcept qualifier (for C compatibility)
// so c_call_internal does instead.
TrampolineResult
c_call_0args(void *function_pointer, void *host_call_context)
{
  return hogd::c_call_internal(function_pointer, host_call_context);
}

TrampolineResult
c_call_1args(void *function_pointer, void *host_call_context, uint64_t arg1)
{
  return hogd::c_call_internal(function_pointer, host_call_context, arg1);
}

TrampolineResult
c_call_2args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2)
{
  return hogd::c_call_internal(function_pointer, host_call_context, arg1, arg2);
}

TrampolineResult
c_call_3args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3)
{
  return hogd::c_call_internal(function_pointer, host_call_context, arg1, arg2,
                               arg3);
}

TrampolineResult
c_call_4args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
  return hogd::c_call_internal(function_pointer, host_call_context, arg1, arg2,
                               arg3, arg4);
}

TrampolineResult
c_call_5args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
  return hogd::c_call_internal(function_pointer, host_call_context, arg1, arg2,
                               arg3, arg4, arg5);
}

TrampolineResult
c_call_6args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
             uint64_t arg6)
{
  return hogd::c_call_internal(function_pointer, host_call_context, arg1, arg2,
                               arg3, arg4, arg5, arg6);
}

TrampolineResult
c_call_7args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
             uint64_t arg6, uint64_t arg7)
{
  return hogd::c_call_internal(function_pointer, host_call_context, arg1, arg2,
                               arg3, arg4, arg5, arg6, arg7);
}

TrampolineResult
c_call_8args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
             uint64_t arg6, uint64_t arg7, uint64_t arg8)
{
  return hogd::c_call_internal(function_pointer, host_call_context, arg1, arg2,
                               arg3, arg4, arg5, arg6, arg7, arg8);
}

} // extern C
