#include "hogd/hogexec/runtime/syscall_inner.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringGet_inner(SystemCallHandler &syscall_handler,
                            const StorageKey &input_key,
                            std::span<std::byte> output_bytestring,
                            RuntimeMemoryLength &output_written_len)
{
  auto status = syscall_handler.get_bytestring(input_key);
  if (!status) {
    return {status.error().code(), 0};
  }

  output_written_len =
      std::min<RuntimeMemoryLength>(output_bytestring.size(), status->size());

  std::memcpy(reinterpret_cast<uint8_t *>(output_bytestring.data()),
              status->data(), output_written_len);

  return {SyscallErrorCode::kNone, 0};
};

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringGetLen_inner(SystemCallHandler &syscall_handler,
                               const StorageKey &input_key,
                               RuntimeMemoryLength &output_length)
{
  auto status = syscall_handler.get_bytestring(input_key);
  if (!status) {
    return {status.error().code(), 0};
  }

  output_length = (*status).size();

  return {SyscallErrorCode::kNone, 0};
};

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringSet_inner(SystemCallHandler &syscall_handler,
                            const StorageKey &input_key,
                            std::span<const std::byte> input_bytestring)
{
  auto status = syscall_handler.set_bytestring(input_key, input_bytestring);
  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
