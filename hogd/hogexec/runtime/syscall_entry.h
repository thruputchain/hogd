#pragma once

#include "hogd/hogexec/runtime/status.h"

namespace hogd {

struct HostCallContext;

HostFnStatus<int32_t> syscall_entry(HostCallContext *call_context,
                                    uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                    uint64_t arg3, uint64_t arg4, uint64_t arg5,
                                    uint64_t arg6);

} // namespace hogd
