#pragma once

#include <span>
#include <utility>

#include "hogd/hog_exec/common/status.h"
#include "x23/async.h"

namespace hogd {

class SystemCallHandler;

struct ExecutionContext {
  std::span<std::byte> memory;
  uint64_t gas_remaining;
};

using ExternHostCallContext = void *;

// What a syscall produces: the host-function status the sandbox acts on, plus
// the gas actually charged.
using SyscallResult = std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>;

// Either a SyscallResult outright or an Async the caller co_awaits (or blocks
// on with operator()). Value arm: the body returned a plain result and ran with
// no coroutine. Async arm: the body might wait (storage reads, deploy, invoke).
using MaybeAsyncSyscallResult = x23::MaybeAsync<SyscallResult>;

using SyscallFunction = MaybeAsyncSyscallResult (*)(
    ExternHostCallContext, ExecutionContext, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

} // namespace hogd
