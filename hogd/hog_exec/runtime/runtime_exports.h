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

// Syscalls are coroutinized: this is either a SyscallResult outright or an
// Async the caller co_awaits (or, from a synchronous caller, blocks on with
// operator()).
//
// Today it is always the Async arm. wrap_after_parse_async() is a coroutine
// unconditionally, so every syscall allocates a frame even though no inner
// body suspends yet; the value arm here only starts carrying traffic once that
// wrapper stops being a coroutine for bodies that cannot wait. The value arm
// on MaybeAsyncSyscallInnerResult, one layer down, is the one that is free
// today -- see syscall_types.h.
using MaybeAsyncSyscallResult = x23::MaybeAsync<SyscallResult>;

using SyscallFunction = MaybeAsyncSyscallResult (*)(
    ExternHostCallContext, ExecutionContext, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

} // namespace hogd
