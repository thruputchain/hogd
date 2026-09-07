#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <cassert>
#include <optional>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
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

namespace {

using NNIntReadResult =
    SystemStatus<SyscallStatus<std::pair<std::optional<int128_t>, int128_t>>>;

SyscallInnerResult
finish_get(SystemCallHandler &syscall_handler, NNIntReadResult status,
           bool input_force_load_from_snapshot, int128_t &output_current_set,
           bool &output_current_set_opt, int128_t &output_current_add)
{
  if (!status) {
    return SyscallInnerResult{status.error().to_host_fn_error(),
                              WrappedGasAmount{0}};
  }
  if (!(*status).has_value()) {
    return SyscallInnerResult{(*status).error().code(), WrappedGasAmount{0}};
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
  return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                            WrappedGasAmount{kStorageAccessCost}};
}

x23::Async<SyscallInnerResult>
get_slow(SystemCallHandler &handler, x23::MaybeAsync<NNIntReadResult> pending,
         bool force_snapshot, int128_t &output_set, bool &output_set_opt,
         int128_t &output_add)
{
  co_return finish_get(handler, co_await pending, force_snapshot, output_set,
                       output_set_opt, output_add);
}

SyscallInnerResult
finish_set(SystemStatus<SyscallStatus<void>> status)
{
  if (!status.has_value()) {
    return SyscallInnerResult{status.error().to_host_fn_error(),
                              WrappedGasAmount{0}};
  }
  if (!(*status).has_value()) {
    return SyscallInnerResult{(*status).error().code(), WrappedGasAmount{0}};
  }

  return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                            WrappedGasAmount{kStorageAccessCost}};
}

x23::Async<SyscallInnerResult>
set_slow(x23::MaybeAsync<SystemStatus<SyscallStatus<void>>> pending)
{
  co_return finish_set(co_await pending);
}

} // namespace

MaybeAsyncSyscallInnerResult
syscall_NonnegativeIntGet_inner(SystemCallHandler &syscall_handler,
                                const StorageKeyContractData &input_key,
                                bool input_force_load_from_snapshot,
                                int128_t &output_current_set,
                                bool &output_current_set_opt,
                                int128_t &output_current_add)
{
  auto pending =
      syscall_handler.get_nnint(input_key, input_force_load_from_snapshot);
  // Keep cache hits immediate all the way through the syscall wrapper.
  if (pending.has_value()) {
    return finish_get(syscall_handler, pending(),
                      input_force_load_from_snapshot, output_current_set,
                      output_current_set_opt, output_current_add);
  }
  return get_slow(syscall_handler, std::move(pending),
                  input_force_load_from_snapshot, output_current_set,
                  output_current_set_opt, output_current_add);
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
  auto pending = syscall_handler.set_nnint(input_key, set_value,
                                           input_add_value, input_flags);
  if (pending.has_value()) {
    return finish_set(pending());
  }
  return set_slow(std::move(pending));
}

} // namespace hogd
