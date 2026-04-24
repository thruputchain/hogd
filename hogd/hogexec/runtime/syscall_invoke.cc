#include "hogd/hogexec/runtime/syscall_inner.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <exception>
#include <span>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "invoke_flag.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetMsgSender_inner(SystemCallHandler &syscall_handler,
                           ContractAddress &output_sender) noexcept
{
  auto status = syscall_handler.get_msg_sender();
  if (!status) {
    return {status.error().code(), WrappedGasAmount{0}};
  }

  output_sender = *status;
  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kGetMsgSender>()) {
    try {
      trace_data->outputs.emplace(
          SyscallGetMsgSenderOutputs{.sender = output_sender});
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kGetMsgSender,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetCurrentAddress_inner(SystemCallHandler &syscall_handler,
                                ContractAddress &output_address) noexcept
{
  output_address = syscall_handler.get_current_address();
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetCurrentAddress>()) {
    try {
      trace_data->outputs.emplace(
          SyscallGetCurrentAddressOutputs{.address = output_address});
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kGetCurrentAddress,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetSourceTransactionHash_inner(
    SystemCallHandler &syscall_handler,
    SHA256Hash &output_source_tx_hash) noexcept
{
  output_source_tx_hash = syscall_handler.get_source_tx_hash();
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetSourceTransactionHash>()) {
    try {
      trace_data->outputs.emplace(SyscallGetSourceTransactionHashOutputs{
          .source_tx_hash = output_source_tx_hash});
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}",
            SyscallNo::kGetSourceTransactionHash, e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetCalldataLen_inner(SystemCallHandler &syscall_handler,
                             RuntimeMemoryLength &output_len) noexcept
{
  output_len = syscall_handler.get_calldata().size();
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetCalldataLen>()) {
    try {
      trace_data->outputs.emplace(
          SyscallGetCalldataLenOutputs{.len = output_len});
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kGetCalldataLen,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetCalldata_inner(SystemCallHandler &syscall_handler,
                          std::span<std::byte> output_buffer,
                          RuntimeMemoryLength &output_written_len) noexcept
{
  auto const &calldata = syscall_handler.get_calldata();
  output_written_len =
      std::min<RuntimeMemoryLength>(calldata.size(), output_buffer.size());
  if (output_written_len > 0) {
    assert(output_buffer.data() != nullptr);
    assert(calldata.data() != nullptr);
    std::memcpy(reinterpret_cast<uint8_t *>(output_buffer.data()),
                calldata.data(), output_written_len);
  }

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kGetCalldata>()) {
    try {
      trace_data->outputs.emplace(
          SyscallGetCalldataOutputs{.calldata = calldata});
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kGetCalldata,
            e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_Invoke_inner(SystemCallHandler &syscall_handler,
                     WrappedGasAmount remaining_gas,
                     const ContractAddress &input_address,
                     uint64_t input_entrypoint_id, GasAmount input_gas_limit,
                     std::span<const std::byte> input_calldata,
                     InvokeFlagSet input_flags,
                     std::span<const std::byte> input_owned_auth_token_ids,
                     RuntimeMemoryLength &output_return_buffer_len,
                     RuntimeMemoryLength &output_returned_auth_token_len) noexcept
{
  Calldata calldata;
  try {
    calldata.resize_uninitialized(input_calldata.size());
  } catch (std::exception &e) {
    error("unrecoverable: failed to resize calldata: {}", e.what());
    return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
  }
  if (!input_calldata.empty()) {
    assert(calldata.data() != nullptr);
    assert(input_calldata.data() != nullptr);
    std::memcpy(calldata.data(),
                reinterpret_cast<const uint8_t *>(input_calldata.data()),
                input_calldata.size());
  }

  if (input_owned_auth_token_ids.size() % sizeof(uint32_t) != 0) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, WrappedGasAmount{0}};
  }

  std::span<const uint32_t> owned_auth_token_ids(reinterpret_cast<const uint32_t *>(input_owned_auth_token_ids.data()), input_owned_auth_token_ids.size() / sizeof(uint32_t));

  auto [invoke_res, gas_amount] =
      syscall_handler.invoke(input_address, input_entrypoint_id,
                             std::min<GasAmount>(input_gas_limit,
                                                 remaining_gas.get()),
                             calldata, input_flags, owned_auth_token_ids);

  // invocation returned successfully
  if (invoke_res.has_value() &&
      ((*invoke_res) == SyscallErrorCode::kSyscallErrorCodeNone ||
       (*invoke_res) == SyscallErrorCode::kSyscallErrorCodeFrameReverted)) {
    auto return_buffer_host = syscall_handler.get_return_buffer();
    if (!return_buffer_host.has_value()) {
      // Handle the SystemError case
      return {return_buffer_host.error().to_host_fn_error(),
              WrappedGasAmount{gas_amount}};
    }

    SyscallStatus<const ReturnBuffer *> return_buffer = *return_buffer_host;

    if (return_buffer.has_value()) {
      output_return_buffer_len = (*return_buffer)->size();
    }
    else {
      output_return_buffer_len = 0;
    }
    return {*invoke_res, WrappedGasAmount{gas_amount}};
  }
  // Invocation encountered some error
  output_return_buffer_len = 0;
  // TODO(geoff): returning auth tokens unsupported yet
  output_returned_auth_token_len = 0;
  return {invoke_res, WrappedGasAmount{gas_amount}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_Return_inner(SystemCallHandler &syscall_handler,
                     std::span<const std::byte> input_buffer,
                     bool input_revert_frame) noexcept
{
  // If the return buffer is too large, we return a deterministic error`
  // equivalent to a wasm `unreachable` instruction.
  // This is instead of return a kInvalidSyscallInput error.  The generated
  // contract bindings in the SDK for this call has a signature of (&[u8], bool)
  // -> !, which could lead to UB in the contract code if that function then
  // returns an error.
  if (input_buffer.size() > kMaxReturnBufferLen) {
    return {HostFnError{HostFnErrorCode::Exit}, WrappedGasAmount{0}};
  }

  ReturnBuffer buffer;
  try {
    buffer.resize_uninitialized(input_buffer.size());
  } catch (std::exception &e) {
    error("unrecoverable: failed to resize return buffer: {}", e.what());
    return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
  }
  if (!input_buffer.empty()) {
    assert(buffer.data() != nullptr);
    assert(input_buffer.data() != nullptr);
    std::memcpy(buffer.data(),
                reinterpret_cast<const uint8_t *>(input_buffer.data()),
                input_buffer.size());
  }

  syscall_handler.set_return_buffer(std::move(buffer), input_revert_frame);

  return {HostFnError{HostFnErrorCode::Return}, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetReturnedBuffer_inner(
    SystemCallHandler &syscall_handler, std::span<std::byte> output_buffer,
    RuntimeMemoryLength &output_written_len) noexcept
{
  auto status = syscall_handler.get_return_buffer();
  if (!status.has_value()) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  if (!(*status).has_value()) {
    return {status->error().code(), WrappedGasAmount{0}};
  }
  CHECK(**status != nullptr);

  bool written = false;
  size_t write_len = std::min(output_buffer.size(), (**status)->size());
  if (write_len > 0) {
    assert(output_buffer.data() != nullptr);
    assert((**status)->data() != nullptr);
    std::memcpy(output_buffer.data(), (**status)->data(), write_len);
    written = true;
  }
  output_written_len = write_len;

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetReturnedBuffer>()) {
    try {
      if (written) {
        trace_data->outputs.emplace(
            SyscallGetReturnedBufferOutputs{.buffer = ***status});
      }
      else {
        trace_data->outputs.emplace(
            SyscallGetReturnedBufferOutputs{.buffer = ReturnBuffer()});
      }
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kGetReturnedBuffer,
            e.what());
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_Exit_inner() noexcept
{
  return {HostFnError{HostFnErrorCode::Exit}, WrappedGasAmount{0}};
}

} // namespace hogd
