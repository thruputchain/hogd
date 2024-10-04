#include "hogd/hogexec/runtime/syscall_inner.h"

#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_GetMsgSender_inner(SystemCallHandler &syscall_handler,
                           ContractAddress &output_sender)
{
  SyscallStatus<ContractAddress> status = syscall_handler.get_msg_sender();
  if (!status) {
    return {status.error().code(), 0};
  }

  output_sender = *status;

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
