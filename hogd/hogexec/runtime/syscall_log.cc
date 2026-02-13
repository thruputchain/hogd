#include "event.hh"
#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <exception>
#include <span>
#include <string_view>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
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

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kLogEvent>()) {
    try {
      // TODO(geoff): the topics should be included in the trace data
      auto const *d = reinterpret_cast<const uint8_t *>(data.data());
      trace_data->success().data.append(d, d + data.size());
      uint32_t num_topics = 0;
      if (topic0) num_topics++;
      if (topic1) num_topics++;
      if (topic2) num_topics++;
      trace_data->success().num_topics = num_topics;
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kLogEvent, e.what());
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
