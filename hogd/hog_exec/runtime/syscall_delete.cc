#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <utility>

#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_DeleteObject_inner(SystemCallHandler &syscall_handler,
                           const StorageKeyContractData &input_key,
                           StorageDeltaFlagSet input_flags)
{
  auto status = syscall_handler.delete_object(input_key, input_flags);
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  auto &delete_status = *status;
  if (!delete_status) {
    return {delete_status.error().code(), WrappedGasAmount{0}};
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

} // namespace hogd
