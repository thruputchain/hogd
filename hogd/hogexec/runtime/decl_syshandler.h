#pragma once

#include "hogd/base/types.h"

namespace hogd {

uint64_t decl_syshandler(void *call_context, uint64_t arg0,
                           uint64_t arg1, uint64_t arg2, uint64_t arg3,
                           uint64_t arg4, uint64_t arg5, uint64_t arg6) noexcept;
} // namespace hogd
