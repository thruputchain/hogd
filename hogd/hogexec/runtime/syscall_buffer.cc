#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BufferAccumulate_inner(SystemCallHandler &syscall_handler,
                               std::span<const std::byte> input_data,
                               const uint32_t *optional_input_handle,
                               uint32_t &output_handle)
{
  auto status =
      syscall_handler.append_to_buffer(input_data, optional_input_handle);
  if (!status) {
    return {status.error().code(), 0};
  }
  output_handle = *status;
  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
