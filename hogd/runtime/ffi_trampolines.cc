#include "hogd/runtime/ffi_trampolines.h"

#include "hogd/runtime/host_fn_status.h"

#include "hogd/base/types.h"

namespace hogd {

class HostCallContext;

namespace detail {

constexpr static uint8_t kTrampolineErrorNONE = 0;
constexpr static uint8_t kTrampolineErrorHOSTERROR = 1;
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

extern "C" {
TrampolineResult
c_call_0args(void *function_pointer, void *host_call_context) noexcept
{
  try {
    auto *user_ctx =
        reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr =
        (HostFnStatus<uint64_t>(*)(HostCallContext *))(function_pointer);
    HostFnStatus<uint64_t> res = (*ptr)(user_ctx);

    if (res) {
      return detail::success(res.value());
    }
      if (res.error().recoverable()) {
        return detail::error(kTrampolineErrorHOSTERROR);
      }
      return detail::error(kTrampolineErrorUNRECOVERABLE);
    
  } catch (...) {
    // UB to pass errors across FFI boundaries
    return detail::error(kTrampolineErrorUNRECOVERABLE);
  }
}

TrampolineResult
c_call_1args(void *function_pointer, void *host_call_context,
             uint64_t arg1) noexcept
{
  try {
    auto *user_ctx =
        reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr = (HostFnStatus<uint64_t>(*)(HostCallContext *, uint64_t))(
        function_pointer);
    HostFnStatus<uint64_t> res = (*ptr)(user_ctx, arg1);

    if (res) {
      return detail::success(res.value());
    }
      if (res.error().recoverable()) {
        return detail::error(kTrampolineErrorHOSTERROR);
      }
      return detail::error(kTrampolineErrorUNRECOVERABLE);
    
  } catch (...) {
    // UB to pass errors across FFI boundaries
    return detail::error(kTrampolineErrorUNRECOVERABLE);
  }
}

TrampolineResult
c_call_2args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2) noexcept
{
  try {
    auto *user_ctx =
        reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr = (HostFnStatus<uint64_t>(*)(HostCallContext *, uint64_t,
                                           uint64_t))(function_pointer);
    HostFnStatus<uint64_t> res = (*ptr)(user_ctx, arg1, arg2);

    if (res) {
      return detail::success(res.value());
    }
  if (res.error().recoverable()) {
    return detail::error(kTrampolineErrorHOSTERROR);
  }
  return detail::error(kTrampolineErrorUNRECOVERABLE);

  } catch (...) {
    // UB to pass errors across FFI boundaries
    return detail::error(kTrampolineErrorUNRECOVERABLE);
  }
}

TrampolineResult
c_call_3args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3) noexcept
{
  try {
    auto *user_ctx =
        reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr =
        (HostFnStatus<uint64_t>(*)(HostCallContext *, uint64_t, uint64_t,
                                   uint64_t))(function_pointer);
    HostFnStatus<uint64_t> res = (*ptr)(user_ctx, arg1, arg2, arg3);

    if (res) {
      return detail::success(res.value());
    }
      if (res.error().recoverable()) {
        return detail::error(kTrampolineErrorHOSTERROR);
      }
      return detail::error(kTrampolineErrorUNRECOVERABLE);
    
  } catch (...) {
    // UB to pass errors across FFI boundaries
    return detail::error(kTrampolineErrorUNRECOVERABLE);
  }
}

TrampolineResult
c_call_4args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4) noexcept
{
  try {
    auto *user_ctx =
        reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr =
        (HostFnStatus<uint64_t>(*)(HostCallContext *, uint64_t, uint64_t,
                                   uint64_t, uint64_t))(function_pointer);
    HostFnStatus<uint64_t> res = (*ptr)(user_ctx, arg1, arg2, arg3, arg4);

    if (res) {
      return detail::success(res.value());
    }
      if (res.error().recoverable()) {
        return detail::error(kTrampolineErrorHOSTERROR);
      }
      return detail::error(kTrampolineErrorUNRECOVERABLE);
    
  } catch (...) {
    // UB to pass errors across FFI boundaries
    return detail::error(kTrampolineErrorUNRECOVERABLE);
  }
}

TrampolineResult
c_call_5args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4,
             uint64_t arg5) noexcept
{
  try {
    auto *user_ctx =
        reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr = (HostFnStatus<uint64_t>(*)(HostCallContext *, uint64_t,
                                           uint64_t, uint64_t, uint64_t,
                                           uint64_t))(function_pointer);
    HostFnStatus<uint64_t> res = (*ptr)(user_ctx, arg1, arg2, arg3, arg4, arg5);

    if (res) {
      return detail::success(res.value());
    }
      if (res.error().recoverable()) {
        return detail::error(kTrampolineErrorHOSTERROR);
      }
      return detail::error(kTrampolineErrorUNRECOVERABLE);
    
  } catch (...) {
    // UB to pass errors across FFI boundaries
    return detail::error(kTrampolineErrorUNRECOVERABLE);
  }
}
}

} // namespace detail

} // namespace hogd
