#include "hogd/hogexec/runtime/syscall_inner.h"

#include <exception>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_DeleteObject_inner(SystemCallHandler &syscall_handler,
                           const StorageKeyContractData &input_key,
                           StorageDeltaFlagSet input_flags) noexcept
{
  auto status =
      syscall_handler.delete_object(input_key, input_flags);
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kDeleteObject>()) {
    try {
      trace_data->success().key = input_key;
      trace_data->success().flags = input_flags;
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kDeleteObject,
            e.what());
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
