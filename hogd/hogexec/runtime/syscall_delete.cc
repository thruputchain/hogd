#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstdint>
#include <utility>

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "storage_delta_flags.hh"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_DeleteObject_inner(SystemCallHandler &syscall_handler,
                           const StorageKeyContractData &input_key,
                           int32_t flags)
{
  auto status =
      syscall_handler.delete_object(input_key,
                                    static_cast<StorageItemRPCFlagSet>(flags));
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
