#pragma once

#include <cstdint>

namespace hogd::detail {

extern "C" {
struct TrampolineResult {
  int32_t result;
  uint8_t panic;
};

// These functions get called (from within Wasmi)
// as wrappers around
// the functions given as arguments to WasmiContext::link_fn.
TrampolineResult c_call_0args(void *function_pointer,
                              void *host_call_context) noexcept;
TrampolineResult c_call_1args(void *function_pointer, void *host_call_context,
                              uint64_t arg1) noexcept;
TrampolineResult c_call_2args(void *function_pointer, void *host_call_context,
                              uint64_t arg1, uint64_t arg2) noexcept;
TrampolineResult c_call_3args(void *function_pointer, void *host_call_context,
                              uint64_t arg1, uint64_t arg2,
                              uint64_t arg3) noexcept;
TrampolineResult c_call_4args(void *function_pointer, void *host_call_context,
                              uint64_t arg1, uint64_t arg2, uint64_t arg3,
                              uint64_t arg4) noexcept;
TrampolineResult c_call_5args(void *function_pointer, void *host_call_context,
                              uint64_t arg1, uint64_t arg2, uint64_t arg3,
                              uint64_t arg4, uint64_t arg5) noexcept;
TrampolineResult c_call_6args(void *function_pointer, void *host_call_context,
                              uint64_t arg1, uint64_t arg2, uint64_t arg3,
                              uint64_t arg4, uint64_t arg5, uint64_t arg6) noexcept;
TrampolineResult c_call_7args(void *function_pointer, void *host_call_context,
                              uint64_t arg1, uint64_t arg2, uint64_t arg3,
                              uint64_t arg4, uint64_t arg5, uint64_t arg6, uint64_t arg7) noexcept;
TrampolineResult c_call_8args(void *function_pointer, void *host_call_context,
                              uint64_t arg1, uint64_t arg2, uint64_t arg3,
                              uint64_t arg4, uint64_t arg5, uint64_t arg6, uint64_t arg7, uint64_t arg8) noexcept;
} // extern C

} // namespace hogd::detail
