#include "hogd/hogexec/runtime/syscall_inner.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "ffi_enums.hh"
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

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetCurrentAddress_inner(SystemCallHandler &syscall_handler,
                                ContractAddress &output_address)
{
  output_address = syscall_handler.get_current_address();
  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetSourceTransactionHash_inner(SystemCallHandler &syscall_handler,
                                       SHA256Hash &output_source_tx_hash)
{
  output_source_tx_hash = syscall_handler.get_source_tx_hash();
  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetCalldataLen_inner(SystemCallHandler &syscall_handler,
                             RuntimeMemoryLength &output_len)
{
  output_len = syscall_handler.get_calldata().size();
  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetCalldata_inner(SystemCallHandler &syscall_handler,
                          std::span<std::byte> output_buffer,
                          RuntimeMemoryLength &output_written_len)
{
  auto const &calldata = syscall_handler.get_calldata();
  output_written_len =
      std::min<RuntimeMemoryLength>(calldata.size(), output_buffer.size());
  std::memcpy(reinterpret_cast<uint8_t *>(output_buffer.data()),
              calldata.data(), output_written_len);

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnResult, GasAmount>
syscall_Invoke_inner(SystemCallHandler &syscall_handler,
                     GasAmount remaining_gas,
                     const ContractAddress &input_address,
                     const MethodID input_method,
                     const GasAmount input_gas_limit,
                     std::span<const std::byte> input_calldata,
                     const InvocationFlagSet input_flags)
{
  Calldata calldata;
  calldata.resize_uninitialized(input_calldata.size());
  std::memcpy(calldata.data(),
              reinterpret_cast<const uint8_t *>(input_calldata.data()),
              input_calldata.size());

  auto [invoke_res, gas_amount] =
      syscall_handler.invoke(Invocation(input_address, input_method,
                                        input_gas_limit, calldata),
                             remaining_gas, input_flags);

  if (invoke_res) {
    return {{HostFnErrorCode::kNoneOrRecoverable,
             static_cast<int32_t>(*invoke_res)},
            gas_amount};
  }
  return {{invoke_res.error().code(), 0}, gas_amount};
}

} // namespace hogd
