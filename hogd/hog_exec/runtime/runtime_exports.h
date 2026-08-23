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

// An Async the caller co_awaits (or blocks on with operator()).  Not a
// MaybeAsync: its variant would be destroyed by whoever resumes last, which on
// the RPC path is the reader worker running inside the completing coroutine's
// own resumption chain -- and destroying the Async arm there destroys a handle
// that is no longer a frame.
using AsyncSyscallResult = x23::Async<SyscallResult>;

using SyscallFunction = AsyncSyscallResult (*)(ExternHostCallContext,
                                               ExecutionContext, uint32_t,
                                               uint32_t, uint32_t, uint32_t,
                                               uint32_t, uint32_t, uint32_t,
                                               uint32_t);

} // namespace hogd
