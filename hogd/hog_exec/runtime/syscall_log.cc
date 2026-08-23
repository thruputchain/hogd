#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <string_view>

#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall_error.hh"

namespace hogd {

AsyncSyscallInnerResult
syscall_ConsoleLog_inner(std::span<const std::byte> input_buffer)
{
  // Logging the contract-controlled payload formats into heap-backed buffers
  // and can throw (typically std::bad_alloc out of memory); wrap_after_parse's
  // exception boundary translates that to Unrecoverable instead of pretending
  // the syscall succeeded (issue #2717).
  trace("ConsoleLog: {}",
        std::string_view(reinterpret_cast<const char *>(input_buffer.data()),
                         input_buffer.size()));

  // TODO(geoff): maybe entirely disable this call in release builds?  Unclear
  // if spinning on this could be e.g. a DoS vector, if we charge 0 gas forit.
  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{0}};
};

AsyncSyscallInnerResult
syscall_LogEvent_inner(SystemCallHandler &syscall_handler, Topic const *topic0,
                       Topic const *topic1, Topic const *topic2,
                       std::span<const std::byte> data)
{
  if ((topic1 && !topic0) || (topic2 && (!topic1 || !topic0))) {
    // can't have a topicN without topicN-1
    co_return SyscallInnerResult{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
        WrappedGasAmount{0}};
  }

  auto r = syscall_handler.log_event(topic0, topic1, topic2, data);
  if (!r.has_value()) {
    co_return SyscallInnerResult{r.error().code(), WrappedGasAmount{0}};
  }

  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{0}};
}

} // namespace hogd
