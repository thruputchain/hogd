#include "hogd/runtime/ffi_trampolines.h"

#include "hogd/runtime/host_fn_status.h"

#include "hogd/base/types.h"

namespace hogd {

class HostCallContext;

namespace detail {

constexpr static uint8_t kTrampolineErrorNONE = 0;
constexpr static uint8_t kTrampolineErrorOUTOFGAS = 1;
constexpr static uint8_t kTrampolineErrorUNRECOVERABLE = 2;

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

TrampolineResult handle_result(HostFnStatus<uint64_t> const& res) {
  if (res) {
    return success(res.value());
  }

  if (res.error().code() == HostFnErrorCode::kOutOfGas) {
    return error(kTrampolineErrorOUTOFGAS);
  }
  return error(kTrampolineErrorUNRECOVERABLE);
}

extern "C" {
TrampolineResult
c_call_0args(void *function_pointer, void *host_call_context) noexcept
{
  try {
    auto *user_ctx = reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr =
        (HostFnStatus<uint64_t>(*)(HostCallContext *))(function_pointer);

    return handle_result((*ptr)(user_ctx));

  } catch (...) {
    // UB to pass errors across FFI boundaries
    return error(kTrampolineErrorUNRECOVERABLE);
  }
}

TrampolineResult
c_call_1args(void *function_pointer, void *host_call_context,
             uint64_t arg1) noexcept
{
  try {
    auto *user_ctx = reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr = (HostFnStatus<uint64_t>(*)(HostCallContext *, uint64_t))(
        function_pointer);

    return handle_result((*ptr)(user_ctx, arg1));

  } catch (...) {
    // UB to pass errors across FFI boundaries
    return error(kTrampolineErrorUNRECOVERABLE);
  }
}

TrampolineResult
c_call_2args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2) noexcept
{
  try {
    auto *user_ctx = reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr = (HostFnStatus<uint64_t>(*)(HostCallContext *, uint64_t,
                                           uint64_t))(function_pointer);
    
    return handle_result((*ptr)(user_ctx, arg1, arg2));

  } catch (...) {
    // UB to pass errors across FFI boundaries
    return error(kTrampolineErrorUNRECOVERABLE);
  }
}

TrampolineResult
c_call_3args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3) noexcept
{
  try {
    auto *user_ctx = reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr =
        (HostFnStatus<uint64_t>(*)(HostCallContext *, uint64_t, uint64_t,
                                   uint64_t))(function_pointer);
    return handle_result((*ptr)(user_ctx, arg1, arg2, arg3));

  } catch (...) {
    // UB to pass errors across FFI boundaries
    return error(kTrampolineErrorUNRECOVERABLE);
  }
}

TrampolineResult
c_call_4args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4) noexcept
{
  try {
    auto *user_ctx = reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr =
        (HostFnStatus<uint64_t>(*)(HostCallContext *, uint64_t, uint64_t,
                                   uint64_t, uint64_t))(function_pointer);

    return handle_result((*ptr)(user_ctx, arg1, arg2, arg3, arg4));

  } catch (...) {
    // UB to pass errors across FFI boundaries
    return error(kTrampolineErrorUNRECOVERABLE);
  }
}

TrampolineResult
c_call_5args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4,
             uint64_t arg5) noexcept
{
  try {
    auto *user_ctx = reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr = (HostFnStatus<uint64_t>(*)(HostCallContext *, uint64_t,
                                           uint64_t, uint64_t, uint64_t,
                                           uint64_t))(function_pointer);
    
    return handle_result((*ptr)(user_ctx, arg1, arg2, arg3, arg4, arg5));

  } catch (...) {
    // UB to pass errors across FFI boundaries
    return error(kTrampolineErrorUNRECOVERABLE);
  }
}

} // namespace detail

} // namespace hogd
