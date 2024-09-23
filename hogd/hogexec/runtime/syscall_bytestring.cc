#include "hogd/hogexec/runtime/syscall_inner.h"

#include <utility>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_BytestringGet_inner(SystemCallHandler &syscall_handler,
                            const StorageKey &input_key,
                            std::span<std::byte> output_bytestring,
                            uint64_t &output_written_len)
{
  SyscallStatus<const Bytestring> status =
      syscall_handler.get_bytestring(input_key);
  if (!status) {
    return {status.error().code(), 0};
  }

  output_written_len =
      std::min<uint64_t>(output_bytestring.size(), status->size());

  std::memcpy(reinterpret_cast<uint8_t *>(output_bytestring.data()),
              status->data(), output_written_len);

  return {SyscallErrorCode::kNone, 0};
};

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_BytestringGetLen_inner(SystemCallHandler &syscall_handler,
                               const StorageKey &input_key,
                               uint64_t &output_length)
{
  SyscallStatus<uint64_t> status =
      syscall_handler.get_bytestring_len(input_key);
  if (!status) {
    return {status.error().code(), 0};
  }

  output_length = *status;

  return {SyscallErrorCode::kNone, 0};
};

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_BytestringSet_inner(SystemCallHandler &syscall_handler,
                            const StorageKey &input_key,
                            std::span<const std::byte> input_bytestring)
{
  SyscallStatus<void> status =
      syscall_handler.set_bytestring(input_key, input_bytestring);
  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
