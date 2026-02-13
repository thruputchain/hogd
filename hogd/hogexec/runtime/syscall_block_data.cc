#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <cstring>
#include <exception>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetBlockHeight_inner(SystemCallHandler &syscall_handler,
                             BlockHeight &output_result) noexcept
{
  auto status = syscall_handler.get_block_height();
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  output_result = *status;
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetBlockHeight>()) {
    try {
      trace_data->outputs.emplace(SyscallGetBlockHeightOutputs{
        .result = output_result
      });
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kGetBlockHeight,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
