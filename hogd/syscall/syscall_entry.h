#pragma once

#include "hogd/runtime/status.h"

namespace hogd {

class HostCallContext;

HostFnStatus<uint64_t> syscall_entry(HostCallContext *call_context,
                                     uint64_t arg0, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3,
                                     uint64_t arg4);

} // namespace hogd
