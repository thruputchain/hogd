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

// NOLINTBEGIN(misc-unused-parameters)
std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_LogEvent_inner(SystemCallHandler &syscall_handler,
                       SHA256Hash const &topic0, SHA256Hash const &topic1,
                       SHA256Hash const &topic2,
                       std::span<const std::byte> data)
{
  // unimplemented
  return {SyscallErrorCode::kSyscallErrorCodeNoSyscall, 0};
}
// NOLINTEND(misc-unused-parameters)

} // namespace hogd
