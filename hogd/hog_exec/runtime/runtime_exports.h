#pragma once

#include <cstdint>
#include <span>
#include <utility>

#include "hogd/hog_exec/common/status.h"
#include "x23/async.h"

namespace hogd {

class SystemCallHandler;

struct ExecutionContext {
  std::span<std::byte> memory;
  uint64_t gas_remaining;
  // The syscall API minor the calling guest was built against. syscall_entry
  // answers a syscall introduced after it as it does an unknown number, so a
  // guest reaches only the syscalls of the API it declared. Unstated, it is
  // 0: the syscalls every guest of this major may call.
  uint32_t syscall_api_minor = 0;
};

using ExternHostCallContext = void *;

// What a syscall produces: the host-function status the sandbox acts on, plus
// the gas actually charged.
using SyscallResult = std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>;

// Either a SyscallResult outright or an Async the caller co_awaits (or blocks
// on with operator()). Always the Async arm today: wrap_after_parse_async() is
// a coroutine unconditionally, so every syscall allocates a frame.
using MaybeAsyncSyscallResult = x23::MaybeAsync<SyscallResult>;

using SyscallFunction = MaybeAsyncSyscallResult (*)(
    ExternHostCallContext, ExecutionContext, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

} // namespace hogd
