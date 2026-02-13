#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_ConsoleLog_inner(std::span<const std::byte> input_buffer) noexcept
{
  trace("ConsoleLog: {}",
        std::string_view(reinterpret_cast<const char *>(input_buffer.data()),
                         input_buffer.size()));

  // TODO(geoff): maybe entirely disable this call in release builds?  Unclear
  // if spinning on this could be e.g. a DoS vector, if we charge 0 gas forit.
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
};

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_LogEvent_inner(SystemCallHandler &syscall_handler, Topic const *topic0,
                       Topic const *topic1, Topic const *topic2,
                       std::span<const std::byte> data) noexcept
{
  if ((topic1 && !topic0) || (topic2 && (!topic1 || !topic0))) {
    // can't have a topicN without topicN-1
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0};
  }

  auto r = syscall_handler.log_event(topic0, topic1, topic2, data);
  if (!r.has_value()) {
    return {r.error().code(), 0};
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
