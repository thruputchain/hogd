#include "hogd/hogexec/runtime_trampoline/ffi_trampolines.h"

#include <concepts>
#include <cstdint>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "syscall.hh"

namespace hogd {

class HostCallContext;

namespace {

constexpr TrampolineResult
success(int32_t result)
{
  return TrampolineResult{.result = result,
                          .panic = std::to_underlying(
                              HostFnErrorCode::kHostNoneOrRecoverable)};
}

constexpr TrampolineResult
host_exit()
{
  return TrampolineResult{.result = 1,
                          .panic =
                              std::to_underlying(HostFnErrorCode::kHostExit)};
}

constexpr TrampolineResult
error(HostFnErrorCode error_code)
{
  return TrampolineResult{.result = 0, .panic = std::to_underlying(error_code)};
}

constexpr TrampolineResult
unrecoverable()
{
  return error(HostFnErrorCode::kHostUnrecoverable);
}

TrampolineResult
handle_result(HostFnStatus<SyscallErrorCode> const &res)
{
  if (res) {
    return success(std::to_underlying(res.value()));
  }
  switch (res.error().code()) {
  case HostFnErrorCode::kHostOutOfGas:
  case HostFnErrorCode::kHostReturn:
  case HostFnErrorCode::kHostValidationFailure:
  case HostFnErrorCode::kHostAbortTransaction:
    return error(res.error().code());
  case HostFnErrorCode::kHostExit:
    return host_exit();
  default:
    return unrecoverable();
  }
}

template<std::same_as<uint64_t>... Args>
TrampolineResult
c_call_internal(void *function_pointer, void *host_call_context,
                Args... args) noexcept
{
  try {
    auto *user_ctx = reinterpret_cast<HostCallContext *>(host_call_context);
    auto *ptr = reinterpret_cast<HostFnStatus<SyscallErrorCode> (*)(
        HostCallContext *, Args...)>(function_pointer);

    return handle_result((*ptr)(user_ctx, args...));

  } catch (...) { // NOLINT(bugprone-empty-catch)
    return unrecoverable();
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
