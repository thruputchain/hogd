#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <utility>

#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

// Each of these walks the frame stack to the key's item view, as a storage
// write does, and pays the same access cost. None of them reaches the
// database: flags travel with the key's next push.

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_SetStorageFlags_inner(SystemCallHandler &syscall_handler,
                              const StorageKeyContractData &input_key,
                              StorageDeltaFlagSet input_flags)
{
  if (auto status = syscall_handler.set_storage_flags(input_key, input_flags);
      !status.has_value()) {
    return {status.error().code(), WrappedGasAmount{kStorageAccessCost}};
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone,
          WrappedGasAmount{kStorageAccessCost}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetStorageFlags_inner(SystemCallHandler &syscall_handler,
                              const StorageKeyContractData &input_key,
                              StorageDeltaFlagSet &output_flags)
{
  output_flags = syscall_handler.get_storage_flags(input_key);
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetStorageFlags>()) {
    trace_data->outputs.emplace(
        SyscallGetStorageFlagsOutputs{.flags = output_flags});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone,
          WrappedGasAmount{kStorageAccessCost}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_ClearStorageFlags_inner(SystemCallHandler &syscall_handler,
                                const StorageKeyContractData &input_key)
{
  syscall_handler.clear_storage_flags(input_key);
  return {SyscallErrorCode::kSyscallErrorCodeNone,
          WrappedGasAmount{kStorageAccessCost}};
}

} // namespace hogd
