#pragma once

#include <span>

#include "hogd/hog_exec/common/status.h"

namespace hogd {

struct TrampolineResult {
  int32_t result_code;
  int32_t panic; // HostFnErrorCode
  uint64_t gas_consumed;
};

struct ExecutionContext {
  std::span<std::byte> memory;
  uint64_t gas_remaining;
};

// The first parameter is the host call context: a C++ `SystemCallHandler*`
// installed by the host when the sandbox is created and handed back, unchanged,
// to syscall_entry on every host call. Rust treats it as an opaque void*.


using ExternHostCallContext = void*;

using SyscallFunction = std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>(*)(ExternHostCallContext, ExecutionContext, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

}
