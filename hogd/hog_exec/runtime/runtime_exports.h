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

// Syscalls are coroutinized.  One that can answer immediately -- the common
// case -- returns its SyscallResult directly and allocates nothing; one that
// has to wait, e.g. on a database RPC, returns an Async the caller co_awaits
// (or, from a synchronous caller, blocks on with operator()).
using MaybeAsyncSyscallResult = x23::MaybeAsync<SyscallResult>;

using SyscallFunction = MaybeAsyncSyscallResult (*)(
    ExternHostCallContext, ExecutionContext, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

} // namespace hogd
