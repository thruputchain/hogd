#include "hogd/base/log.h"
#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_Log_inner(SystemCallHandler &syscall_handler,
                  std::span<const std::byte> input_buffer)
{
  auto status = syscall_handler.log(input_buffer);
  if (!status) {
    return {status.error().code(), 0};
  }
  trace("Log: {}",
        std::string_view(reinterpret_cast<const char *>(input_buffer.data()),
                         input_buffer.size()));

  return {SyscallErrorCode::kNone, 0};
};

} // namespace hogd
