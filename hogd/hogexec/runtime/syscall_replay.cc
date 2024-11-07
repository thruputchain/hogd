#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_ReplayCacheInsert_inner(SystemCallHandler &syscall_handler,
                                const StorageKey &input_key,
                                uint64_t input_nonce)
{
  auto status = syscall_handler.replay_cache_insert(input_key, input_nonce);

  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_ReplayCacheClear_inner(SystemCallHandler &syscall_handler,
                               const StorageKey &input_key,
                               uint64_t input_threshold)
{
  auto status = syscall_handler.replay_cache_clear(input_key, input_threshold);

  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
