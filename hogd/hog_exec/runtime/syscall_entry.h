#pragma once

#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"

namespace hogd {

AsyncSyscallResult
syscall_entry(ExternHostCallContext call_context, ExecutionContext context,
              uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
              uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7);

} // namespace hogd
