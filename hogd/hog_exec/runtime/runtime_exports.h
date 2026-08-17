#pragma once

#include <span>

#include "hogd/hog_exec/common/status.h"

namespace hogd {

class SystemCallHandler;

struct ExecutionContext {
  std::span<std::byte> memory;
  uint64_t gas_remaining;
};

using ExternHostCallContext = void *;

using SyscallFunction = std::pair<HostFnStatus<SyscallErrorCode>, GasAmount> (
        *)(ExternHostCallContext, ExecutionContext, uint32_t, uint32_t,
           uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

} // namespace hogd
