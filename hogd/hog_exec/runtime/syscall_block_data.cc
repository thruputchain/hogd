#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <cstddef>
#include <cstring>
#include <utility>

#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetBlockHeight_inner(SystemCallHandler &syscall_handler,
                             BlockHeight &output_result)
{
  output_result = syscall_handler.get_block_height();
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetBlockHeight>()) {
    trace_data->outputs.emplace(
        SyscallGetBlockHeightOutputs{.result = output_result});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetNetworkHash_inner(SystemCallHandler &syscall_handler,
                             NetworkHash &output_network_hash)
{
  output_network_hash = syscall_handler.get_network_hash();
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetNetworkHash>()) {
    trace_data->outputs.emplace(
        SyscallGetNetworkHashOutputs{.network_hash = output_network_hash});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetBlockTimepoint_inner(SystemCallHandler &syscall_handler,
                                TimePoint &output_block_timepoint)
{
  output_block_timepoint = syscall_handler.get_block_timepoint();
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetBlockTimepoint>()) {
    trace_data->outputs.emplace(SyscallGetBlockTimepointOutputs{
        .block_timepoint = output_block_timepoint});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

} // namespace hogd
