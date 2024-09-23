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
syscall_BytestringGet_inner(SystemCallHandler& syscall_handler,const StorageKey& input_key, std::span<std::byte> output_bytestring, uint64_t& output_written_len)
{
  SyscallStatus<const Bytestring> status = syscall_handler.get_bytestring(input_key);
  if (!status) {
    return {status.error().code(), 0};
  }

  output_written_len = std::min<uint64_t>(output_bytestring.size(), status->size());

  std::memcpy(reinterpret_cast<uint8_t*>(output_bytestring.data()), status->data(), output_written_len);

  return {SyscallErrorCode::kNone, 0};
};

} // namespace hogd
