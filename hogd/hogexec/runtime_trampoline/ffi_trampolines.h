#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Translation of HostFnResult/HostFnStatus<SyscallErrorCode> into an FFI-safe
// type.
struct TrampolineResult {
  // if HostFnErrorCode is kNoneOrRecoverable, this is the SyscallErrorCode.
  // Otherwise, this will be 0.
  int32_t result;
  // Corresponds to HostFnErrorCode.
  int32_t panic;
};

// These functions get called (from within Wasmi)
// as wrappers around
// the functions given as arguments to WasmiContext::link_fn.
struct TrampolineResult c_call_0args(void *function_pointer,
                                     void *host_call_context);
struct TrampolineResult c_call_1args(void *function_pointer,
                                     void *host_call_context, uint64_t arg1);
struct TrampolineResult c_call_2args(void *function_pointer,
                                     void *host_call_context, uint64_t arg1,
                                     uint64_t arg2);
struct TrampolineResult c_call_3args(void *function_pointer,
                                     void *host_call_context, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3);
struct TrampolineResult c_call_4args(void *function_pointer,
                                     void *host_call_context, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3,
                                     uint64_t arg4);
struct TrampolineResult c_call_5args(void *function_pointer,
                                     void *host_call_context, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3,
                                     uint64_t arg4, uint64_t arg5);
struct TrampolineResult c_call_6args(void *function_pointer,
                                     void *host_call_context, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3,
                                     uint64_t arg4, uint64_t arg5,
                                     uint64_t arg6);
struct TrampolineResult c_call_7args(void *function_pointer,
                                     void *host_call_context, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3,
                                     uint64_t arg4, uint64_t arg5,
                                     uint64_t arg6, uint64_t arg7);
struct TrampolineResult
c_call_8args(void *function_pointer, void *host_call_context, uint64_t arg1,
             uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
             uint64_t arg6, uint64_t arg7, uint64_t arg8);

#ifdef __cplusplus
} // extern C
#endif
