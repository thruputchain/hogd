#pragma once

#ifdef __cplusplus

#include <cstdint>

namespace hogd::detail {

extern "C" {
#else
#include <stdint.h>
#endif

struct TrampolineResult {
  int32_t result;
  uint8_t panic;
};

// NOLINTBEGIN(readability-identifier-naming)
const static uint8_t kTrampolineErrorNONE = 0;
const static uint8_t kTrampolineErrorOUTOFGAS = 1;
const static uint8_t kTrampolineErrorUNRECOVERABLE = 2;
const static uint8_t kTrampolineErrorRETURN = 3;
const static uint8_t kTrampolineErrorEXIT = 4;
// NOLINTEND(readability-identifier-naming)

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

} // namespace hogd::detail
#endif
