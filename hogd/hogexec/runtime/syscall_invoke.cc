#include "hogd/hogexec/runtime/syscall_inner.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "transaction.hh"
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

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_GetCalldataLen_inner(SystemCallHandler &syscall_handler,
                             RuntimeMemoryLength &output_len)
{
  SyscallStatus<RuntimeMemoryLength> status =
      syscall_handler.get_calldata_len();
  if (!status) {
    return {status.error().code(), 0};
  }
  output_len = *status;
  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_GetCalldata_inner(SystemCallHandler &syscall_handler,
                          std::span<std::byte> output_buffer,
                          RuntimeMemoryLength &output_written_len)
{
  SyscallStatus<Calldata> status = syscall_handler.get_calldata();
  if (!status) {
    return {status.error().code(), 0};
  }

  output_written_len =
      std::min<RuntimeMemoryLength>(status->size(), output_buffer.size());

  std::memcpy(reinterpret_cast<uint8_t *>(output_buffer.data()), status->data(),
              output_written_len);

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_Invoke_inner(SystemCallHandler &syscall_handler,
                     const ContractAddress &input_address,
                     const MethodID input_method,
                     const GasAmount input_gas_limit,
                     std::span<const std::byte> input_calldata,
                     RuntimeMemoryLength &output_return_len)
{
  Calldata calldata;
  calldata.resize_uninitialized(input_calldata.size());
  std::memcpy(calldata.data(),
              reinterpret_cast<const uint8_t *>(input_calldata.data()),
              input_calldata.size());

  auto invoke_res = syscall_handler.invoke(
      Invocation(input_address, input_method, input_gas_limit, calldata));

  output_return_len = syscall_handler.get_defaultzero_return_len();

  return invoke_res;
}

} // namespace hogd
