#pragma once

#include <cstdint>

namespace hogd::detail {

extern "C" {
struct TrampolineResult {
  uint64_t result;
  uint8_t panic;
};

// trampolines
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
} // extern C

} // namespace hogd::detail
