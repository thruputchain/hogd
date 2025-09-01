#include "hogd/hogexec/runtime/syscall_inner.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_GetMsgSender_inner(SystemCallHandler &syscall_handler,
                           ContractAddress &output_sender)
{
  auto status = syscall_handler.get_msg_sender();
  if (!status) {
    return {status.error().code(), 0};
  }

  output_sender = *status;
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
syscall_GetCurrentAddress_inner(SystemCallHandler &syscall_handler,
                                ContractAddress &output_address)
{
  output_address = syscall_handler.get_current_address();
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetSourceTransactionHash_inner(SystemCallHandler &syscall_handler,
                                       SHA256Hash &output_source_tx_hash)
{
  output_source_tx_hash = syscall_handler.get_source_tx_hash();
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetCalldataLen_inner(SystemCallHandler &syscall_handler,
                             RuntimeMemoryLength &output_len)
{
  output_len = syscall_handler.get_calldata().size();
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
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

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_Invoke_inner(SystemCallHandler &syscall_handler,
                     WrappedGasAmount remaining_gas,
                     const ContractAddress &input_address,
                     MethodID input_method, GasAmount input_gas_limit,
                     std::span<const std::byte> input_calldata,
                     InvokeFlagSet input_flags,
                     RuntimeMemoryLength &output_return_buffer_len)
{
  Calldata calldata;
  calldata.resize_uninitialized(input_calldata.size());
  std::memcpy(calldata.data(),
              reinterpret_cast<const uint8_t *>(input_calldata.data()),
              input_calldata.size());

  auto [invoke_res, gas_amount] =
      syscall_handler.invoke(input_address, input_method,
                             std::min<GasAmount>(input_gas_limit,
                                                 remaining_gas.gas_amount_),
                             calldata, input_flags);

  // invocation returned successfully
  if (invoke_res &&
      (invoke_res.value() == SyscallErrorCode::kSyscallErrorCodeNone ||
       invoke_res.value() ==
           SyscallErrorCode::kSyscallErrorCodeFrameReverted)) {
    auto return_buffer = syscall_handler.get_return_buffer();
    if (return_buffer.has_value()) {
      output_return_buffer_len = (*return_buffer)->size();
    }
    else {
      output_return_buffer_len = 0;
    }
    return {invoke_res.value(), gas_amount};
  }
  // Invocation encountered some error
  output_return_buffer_len = 0;
  return {invoke_res, gas_amount};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_Return_inner(SystemCallHandler &syscall_handler,
                     std::span<const std::byte> input_buffer,
                     bool input_revert_frame)
{
  if (input_buffer.size() > kMaxReturnBufferLen) {
    return {HostFnError{HostFnErrorCode::kHostExit},
            static_cast<int32_t>(
                SyscallErrorCode::kSyscallErrorCodeDeterministicWasm)};
  }

  ReturnBuffer buffer;
  buffer.resize_uninitialized(input_buffer.size());
  std::memcpy(buffer.data(),
              reinterpret_cast<const uint8_t *>(input_buffer.data()),
              input_buffer.size());
  syscall_handler.set_return_buffer(std::move(buffer), input_revert_frame);

  return {HostFnError{HostFnErrorCode::kHostReturn}, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetReturnedBuffer_inner(SystemCallHandler &syscall_handler,
                                std::span<std::byte> output_buffer,
                                RuntimeMemoryLength &output_written_len)
{
  auto status = syscall_handler.get_return_buffer();
  if (!status) {
    return {status.error().code(), 0};
  }

  size_t write_len = std::min(output_buffer.size(), (*status)->size());
  std::memcpy(output_buffer.data(), (*status)->data(), write_len);
  output_written_len = write_len;

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_Exit_inner()
{
  return {HostFnError{HostFnErrorCode::kHostExit}, 0};
}

} // namespace hogd
