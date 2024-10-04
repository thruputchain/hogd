#include "hogd/hogexec/runtime_trampoline/ffi_trampolines.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"

namespace hogd {

class HostCallContext;

namespace detail {

constexpr static uint8_t kTrampolineErrorNONE = 0;
constexpr static uint8_t kTrampolineErrorOUTOFGAS = 1;
constexpr static uint8_t kTrampolineErrorUNRECOVERABLE = 2;
constexpr static uint8_t kTrampolineErrorRETURN = 3;

TrampolineResult
success(uint64_t res)
{
  return TrampolineResult{res, kTrampolineErrorNONE};
}

TrampolineResult
error(uint8_t error)
{
  return TrampolineResult{0, error};
}

TrampolineResult
handle_result(HostFnStatus<uint64_t> const &res)
{
  if (res) {
    return success(res.value());
  }

  if (res.error().code() == HostFnErrorCode::kOutOfGas) {
    return error(kTrampolineErrorOUTOFGAS);
  }
  if (res.error().code() == HostFnErrorCode::kReturn) {
    return error(kTrampolineErrorRETURN);
  }
  return error(kTrampolineErrorUNRECOVERABLE);
}

template<std::same_as<uint64_t>... Args>
TrampolineResult
c_call_internal(void *function_pointer, void *host_call_context, Args... args)
{
  try {
    auto *user_ctx = reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr = (HostFnStatus<uint64_t>(*)(HostCallContext *, Args...))(
        function_pointer);

    return handle_result((*ptr)(user_ctx, args...));

  } catch (...) {
    return error(kTrampolineErrorUNRECOVERABLE);
  }
}

extern "C" {

// These functions catch all errors, just in case our code throws an exception.
// It is UB to pass errors across FFI boundaries, so they're all declared
// noexcept.
TrampolineResult
c_call_0args(void *function_pointer, void *host_call_context) noexcept
{
  return c_call_internal(function_pointer, host_call_context);
}

TrampolineResult
c_call_1args(void *function_pointer, void *host_call_context,
             uint64_t arg1) noexcept
{
  return c_call_internal(function_pointer, host_call_context, arg1);
}

TrampolineResult
c_call_2args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2) noexcept
{
  return c_call_internal(function_pointer, host_call_context, arg1, arg2);
}

TrampolineResult
c_call_3args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3) noexcept
{
  return c_call_internal(function_pointer, host_call_context, arg1, arg2, arg3);
}

TrampolineResult
c_call_4args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4) noexcept
{
  return c_call_internal(function_pointer, host_call_context, arg1, arg2, arg3,
                         arg4);
}

TrampolineResult
c_call_5args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4,
             uint64_t arg5) noexcept
{
  return c_call_internal(function_pointer, host_call_context, arg1, arg2, arg3,
                         arg4, arg5);
}

} // extern C

} // namespace detail

} // namespace hogd
