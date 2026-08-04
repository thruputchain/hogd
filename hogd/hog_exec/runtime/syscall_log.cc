#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <cstddef>
#include <exception>
#include <span>
#include <string_view>
#include <utility>

#include "event.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall_error.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_ConsoleLog_inner(std::span<const std::byte> input_buffer) noexcept
{
  // Logging the contract-controlled payload formats into heap-backed buffers.
  // This runs in the noexcept syscall body, so a throw (typically
  // std::bad_alloc out of memory) must not cross the noexcept boundary and
  // terminate the process. Treat the failure as unrecoverable and return a
  // structured error instead of pretending the syscall succeeded (issue #2717).
  try {
    trace("ConsoleLog: {}",
          std::string_view(reinterpret_cast<const char *>(input_buffer.data()),
                           input_buffer.size()));
  } catch (std::exception const &e) {
    report_dropped_log(e.what());
    return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
  } catch (...) {
    report_dropped_log("unknown exception");
    return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
  }

  // TODO(geoff): maybe entirely disable this call in release builds?  Unclear
  // if spinning on this could be e.g. a DoS vector, if we charge 0 gas forit.
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
};

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_LogEvent_inner(SystemCallHandler &syscall_handler, Topic const *topic0,
                       Topic const *topic1, Topic const *topic2,
                       std::span<const std::byte> data) noexcept
{
  if ((topic1 && !topic0) || (topic2 && (!topic1 || !topic0))) {
    // can't have a topicN without topicN-1
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }

  auto r = syscall_handler.log_event(topic0, topic1, topic2, data);
  if (!r.has_value()) {
    return {r.error().code(), WrappedGasAmount{0}};
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

} // namespace hogd
