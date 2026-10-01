#include "hogd/hog_exec/runtime/syscall_inner.h"

#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

MaybeAsyncSyscallInnerResult
syscall_DeleteObject_inner(SystemCallHandler &syscall_handler,
                           const StorageKeyContractData &input_key)
{
  auto status = co_await syscall_handler.delete_object(input_key);
  if (!status) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(),
                                 WrappedGasAmount{0}};
  }
  // The frame walk and first-touch item-view insertion have happened by now, so
  // both syscall-visible exits pay the access, as the storage-flags and NNInt
  // syscalls do. delete_object folds in any retrocausal-queue cleanup cost, so
  // body_gas already covers the historical block teardown block close performs.
  auto &delete_read = *status;
  const WrappedGasAmount body_gas{delete_read.load_gas};
  auto &delete_status = delete_read.result;
  if (!delete_status) {
    co_return SyscallInnerResult{delete_status.error().code(), body_gas};
  }

  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               body_gas};
}

} // namespace hogd
