#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <cstddef>
#include <cstring>
#include <exception>
#include <utility>

#include "ffi_enums.hh"
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
                             BlockHeight &output_result) noexcept
{
  auto status = syscall_handler.get_block_height();
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  output_result = *status;
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetBlockHeight>()) {
    try {
      trace_data->outputs.emplace(
          SyscallGetBlockHeightOutputs{.result = output_result});
    } catch (std::exception &e) {
      report_dropped_syscall_trace(e.what());
      return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

} // namespace hogd
