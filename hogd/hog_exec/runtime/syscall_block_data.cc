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
  auto status = syscall_handler.get_block_height();
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  output_result = *status;
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetBlockHeight>()) {
    trace_data->outputs.emplace(
        SyscallGetBlockHeightOutputs{.result = output_result});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

} // namespace hogd
