#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <cassert>
#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

MaybeAsyncSyscallInnerResult
syscall_NonnegativeIntGet_inner(SystemCallHandler &syscall_handler,
                                const StorageKeyContractData &input_key,
                                bool input_force_load_from_snapshot,
                                int128_t &output_current_set,
                                bool &output_current_set_opt,
                                int128_t &output_current_add)
{
  auto status =
      co_await syscall_handler.get_nnint(input_key,
                                         input_force_load_from_snapshot);
  if (!status) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(),
                                 WrappedGasAmount{0}};
  }
  if (!(*status).has_value()) {
    co_return SyscallInnerResult{(*status).error().code(), WrappedGasAmount{0}};
  }
  if ((*status)->first.has_value()) {
    output_current_set = *((*status)->first);
    output_current_set_opt = true;
  }
  else {
    output_current_set_opt = false;
  }
  output_current_add = (*status)->second;

  // If we load from the snapshot, we must have a set value.
  assert(!input_force_load_from_snapshot || output_current_set_opt);

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kNonnegativeIntGet>()) {
    trace_data->outputs.emplace(SyscallNonnegativeIntGetOutputs());
    if (output_current_set_opt) {
      trace_data->outputs->current_set = to_xdr(output_current_set);
    }
    trace_data->outputs->current_add = to_xdr(output_current_add);
  }
  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{0}};
}

MaybeAsyncSyscallInnerResult
syscall_NonnegativeIntSetAdd_inner(SystemCallHandler &syscall_handler,
                                   const StorageKeyContractData &input_key,
                                   const int128_t *optional_input_set_value,
                                   const int128_t &input_add_value,
                                   StorageDeltaFlagSet input_flags)
{
  std::optional<int128_t> set_value = std::nullopt;
  if (optional_input_set_value) {
    set_value = *optional_input_set_value;
  }
  auto status =
      co_await syscall_handler.set_nnint(input_key, set_value, input_add_value,
                                         input_flags);
  if (!status.has_value()) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(),
                                 WrappedGasAmount{0}};
  }
  if (!(*status).has_value()) {
    co_return SyscallInnerResult{(*status).error().code(), WrappedGasAmount{0}};
  }

  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{0}};
}

} // namespace hogd
