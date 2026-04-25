#pragma once

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/trampoline_result.h"

namespace hogd {

struct HostCallContext;

extern "C" TrampolineResult
syscall_entry(HostCallContext *call_context, uint64_t arg0, uint64_t arg1,
              uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5,
              uint64_t arg6, uint64_t arg7, uint64_t arg8, uint64_t arg9,
              uint64_t arg10, uint64_t arg11) noexcept;

} // namespace hogd
