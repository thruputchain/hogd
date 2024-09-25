#include "hogd/hogexec/runtime/syscall_inner.h"

#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/gas_cost_table.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "storage_object.hh"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_HasKey_inner(SystemCallHandler &syscall_handler,
                     const StorageKey &input_key,
                     StorageObjectType &output_status)
{
  SyscallStatus<StorageObjectType> status =
      syscall_handler.get_object_type(input_key);
  if (!status) {
    return {status.error().code(), kHasKeyCost};
  }

  output_status = *status;

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_DeleteLast_inner(SystemCallHandler &syscall_handler,
                         const StorageKey &input_key)
{
  syscall_handler.delete_last(input_key);

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
