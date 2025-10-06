#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_ConsoleLog_inner(std::span<const std::byte> input_buffer)
{
  trace("ConsoleLog: {}",
        std::string_view(reinterpret_cast<const char *>(input_buffer.data()),
                         input_buffer.size()));

  // TODO(geoff): maybe entirely disable this call in release builds?  Unclear
  // if spinning on this could be e.g. a DoS vector, if we charge 0 gas forit.
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
};

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_LogEvent_inner(SystemCallHandler &syscall_handler,
                       SHA256Hash const *topic0, SHA256Hash const *topic1,
                       SHA256Hash const *topic2,
                       std::span<const std::byte> data)
{
  if ((topic1 && !topic0) || (topic2 && (!topic1 || !topic0))) {
    // can't have a topicN without topicN-1
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0};
  }

  if (data.size() > kMaxLogMessageLen) {
    // log message too long, return error
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0};
  }

  syscall_handler.log_event(topic0, topic1, topic2, data);
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
