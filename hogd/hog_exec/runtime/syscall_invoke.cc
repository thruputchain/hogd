#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/syscall_buffer.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "hogd/hog_exec/runtime/validated_auth_tokens.h"
#include "invoke_args.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetMsgSender_inner(SystemCallHandler &syscall_handler,
                           ContractAddress &output_sender)
{
  auto status = syscall_handler.get_msg_sender();
  if (!status) {
    return {status.error().code(), WrappedGasAmount{0}};
  }

  output_sender = *status;
  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kGetMsgSender>()) {
    trace_data->outputs.emplace(
        SyscallGetMsgSenderOutputs{.sender = output_sender});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetCurrentAddress_inner(SystemCallHandler &syscall_handler,
                                ContractAddress &output_address)
{
  output_address = syscall_handler.get_current_address();
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetCurrentAddress>()) {
    trace_data->outputs.emplace(
        SyscallGetCurrentAddressOutputs{.address = output_address});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetSourceTransactionHash_inner(SystemCallHandler &syscall_handler,
                                       SHA256Hash &output_source_tx_hash)
{
  output_source_tx_hash = syscall_handler.get_source_tx_hash();
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetSourceTransactionHash>()) {
    trace_data->outputs.emplace(SyscallGetSourceTransactionHashOutputs{
        .source_tx_hash = output_source_tx_hash});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetCalldataLen_inner(SystemCallHandler &syscall_handler,
                             RuntimeMemoryLength &output_len,
                             uint32_t &output_auth_token_count)
{
  output_len = syscall_handler.get_calldata().size();
  output_auth_token_count =
      syscall_handler.get_parent_supplied_auth_tokens().size();
  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetCalldataLen>()) {
    trace_data->outputs.emplace(SyscallGetCalldataLenOutputs{
        .len = output_len, .auth_token_count = output_auth_token_count});
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetCalldata_inner(SystemCallHandler &syscall_handler,
                          std::span<std::byte> output_buffer,
                          RuntimeMemoryLength &output_written_len,
                          std::span<std::byte> output_auth_token_list,
                          RuntimeMemoryLength &output_auth_token_written_len)
{
  if (output_auth_token_list.size() % sizeof(uint32_t) != 0) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }

  auto const &calldata = syscall_handler.get_calldata();
  auto const calldata_bytes =
      std::as_bytes(std::span(calldata.data(), calldata.size()));
  output_written_len = copy_to_guest_buffer(output_buffer, calldata_bytes);

  auto span_auth_token_list = syscall_handler.get_parent_supplied_auth_tokens();
  auto const auth_token_bytes = std::as_bytes(span_auth_token_list);
  output_auth_token_written_len =
      copy_to_guest_buffer(output_auth_token_list, auth_token_bytes);

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kGetCalldata>()) {
    x23::Vec<uint32_t, 0xffffffff> auth_token_list;
    auth_token_list.append_range(span_auth_token_list);
    trace_data->outputs.emplace(
        SyscallGetCalldataOutputs{.calldata = calldata,
                                  .auth_tokens = auth_token_list});
  }

  // Charge the bytes actually copied into guest memory, so repeated retrieval
  // of a large frame costs more than a tiny one; the frame data needs no
  // storage access, so only the copy term applies (#4040).
  const WrappedGasAmount gas{GasCost::guest_buffer_copy(
      std::min(output_buffer.size(), calldata_bytes.size()) +
      std::min(output_auth_token_list.size(), auth_token_bytes.size()))};
  return {SyscallErrorCode::kSyscallErrorCodeNone, gas};
}

MaybeAsyncSyscallInnerResult
syscall_Invoke_inner(SystemCallHandler &syscall_handler,
                     WrappedGasAmount remaining_gas,
                     const ContractAddress &input_address,
                     std::span<const std::byte> input_calldata,
                     std::span<const std::byte> input_owned_auth_token_ids,
                     const CopyableInvokeInputs &input_inputs,
                     CopyableInvokeOutputs &output_outputs)
{
  if (!SystemCallHandler::validate_invoke_flags(input_inputs.flags)) {
    co_return SyscallInnerResult{
        SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
        WrappedGasAmount{0}};
  }

  // Materialized once; the frame stack takes the list over without copying.
  auto owned_auth_tokens =
      ValidatedAuthTokens::parse(input_owned_auth_token_ids);
  if (!owned_auth_tokens.has_value()) {
    co_return SyscallInnerResult{owned_auth_tokens.error().code(),
                                 WrappedGasAmount{0}};
  }

  // Admission before the calldata is copied: a rejection reports gas 0, so
  // copying first would be unpriced, length-proportional work a contract could
  // repeat at will (#2973). invoke() relies on this having run.
  auto admitted =
      syscall_handler.validate_new_invocation(input_address, input_inputs.flags,
                                              *owned_auth_tokens);
  if (!admitted.has_value()) {
    co_return SyscallInnerResult{admitted.error().code(), WrappedGasAmount{0}};
  }

  Calldata calldata;
  calldata.resize_uninitialized(input_calldata.size());
  if (!input_calldata.empty()) {
    assert(calldata.data() != nullptr);
    assert(input_calldata.data() != nullptr);
    std::memcpy(calldata.data(),
                reinterpret_cast<const uint8_t *>(input_calldata.data()),
                input_calldata.size());
  }

  auto [invoke_res, gas_amount] = co_await syscall_handler.invoke(
      input_address, input_inputs.entrypoint_id,
      std::min<GasAmount>(input_inputs.gas_limit, remaining_gas.get()),
      std::move(calldata), input_inputs.flags, std::move(*owned_auth_tokens));

  // A SystemError ends execution of the whole block.
  if (!invoke_res.has_value()) {
    co_return SyscallInnerResult{invoke_res.error().to_host_fn_error(),
                                 WrappedGasAmount{gas_amount}};
  }

  // A SyscallError means the invoke was rejected before a child frame ran: a
  // complete no-op. Leave the caller's output struct untouched (mirroring every
  // other syscall) and report the error code.
  SyscallStatus<InvocationFrameStatusCode> const &frame = *invoke_res;
  if (!frame.has_value()) {
    co_return SyscallInnerResult{frame.error().code(),
                                 WrappedGasAmount{gas_amount}};
  }

  // A child frame ran. Report its outcome (Return / Abort / Fault) via
  // frame_status; the syscall error code stays None. The return buffer is
  // readable on Return / Abort and absent on Fault.
  auto return_buffer_host = syscall_handler.get_return_buffer();
  if (!return_buffer_host.has_value()) {
    // Handle the SystemError case
    co_return SyscallInnerResult{return_buffer_host.error().to_host_fn_error(),
                                 WrappedGasAmount{gas_amount}};
  }

  SyscallStatus<const ReturnBuffer *> return_buffer = *return_buffer_host;
  if (return_buffer.has_value()) {
    output_outputs.return_buffer_len = (*return_buffer)->size();
  }
  else {
    output_outputs.return_buffer_len = 0;
  }
  output_outputs.returned_auth_token_len =
      syscall_handler.get_returned_auth_token_ids().size() * sizeof(uint32_t);
  // operator* (not value()) so this stays noexcept: has_value() is checked
  // above, so the dereference is safe.
  output_outputs.frame_status = *frame;
  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{gas_amount}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_Return_inner(SystemCallHandler &syscall_handler,
                     std::span<const std::byte> input_buffer,
                     bool input_revert_frame,
                     std::span<const std::byte> input_returned_auth_token_ids)
{
  // If the return buffer is too large, we return a deterministic error`
  // equivalent to a guest trap.
  // This is instead of return a kInvalidSyscallInput error.  The generated
  // contract bindings in the SDK for this call has a signature of (&[u8], bool)
  // -> !, which could lead to UB in the contract code if that function then
  // returns an error.
  if (input_buffer.size() > kMaxReturnBufferLen) {
    return {HostFnError{HostFnErrorCode::Exit}, WrappedGasAmount{0}};
  }

  if (!input_returned_auth_token_ids.empty() && input_revert_frame) {
    // Cannot revert the frame and also return auth tokens
    return {HostFnError{HostFnErrorCode::Exit}, WrappedGasAmount{0}};
  }

  // Ordered ids go to the parent; set() only serves the duplicate check.
  auto returned_auth_tokens =
      ValidatedAuthTokens::parse(input_returned_auth_token_ids);
  if (!returned_auth_tokens.has_value()) {
    return {HostFnError{HostFnErrorCode::Exit}, WrappedGasAmount{0}};
  }

  if (!syscall_handler.check_auth_tokens_owned(returned_auth_tokens->set())) {
    return {HostFnError{HostFnErrorCode::Exit}, WrappedGasAmount{0}};
  }

  ReturnBuffer buffer;
  buffer.resize_uninitialized(input_buffer.size());
  if (!input_buffer.empty()) {
    assert(buffer.data() != nullptr);
    assert(input_buffer.data() != nullptr);
    std::memcpy(buffer.data(),
                reinterpret_cast<const uint8_t *>(input_buffer.data()),
                input_buffer.size());
  }

  syscall_handler.set_return_buffer(
      std::move(buffer), input_revert_frame,
      std::move(*returned_auth_tokens).release().first);

  return {HostFnError{HostFnErrorCode::Return}, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_GetReturnedBuffer_inner(
    SystemCallHandler &syscall_handler, std::span<std::byte> output_buffer,
    RuntimeMemoryLength &output_written_len,
    std::span<std::byte> output_restored_auth_token_ids,
    RuntimeMemoryLength &output_restored_auth_token_ids_written_len)
{
  auto status = syscall_handler.get_return_buffer();
  if (!status.has_value()) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  if (!(*status).has_value()) {
    return {status->error().code(), WrappedGasAmount{0}};
  }
  CHECK(**status != nullptr);

  if (output_restored_auth_token_ids.size() % sizeof(uint32_t) != 0) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }

  auto const &return_buffer = ***status;
  output_written_len =
      copy_to_guest_buffer(output_buffer,
                           std::as_bytes(std::span(return_buffer.data(),
                                                   return_buffer.size())));
  auto returned_ids = syscall_handler.get_returned_auth_token_ids();
  output_restored_auth_token_ids_written_len =
      copy_to_guest_buffer(output_restored_auth_token_ids,
                           std::as_bytes(returned_ids));

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kGetReturnedBuffer>()) {
    // Trace the full return buffer (pre-truncation), regardless of how much
    // fit in the contract's buffer — consistent with the other
    // buffer-writing syscalls, which trace their full output.
    trace_data->outputs.emplace(
        SyscallGetReturnedBufferOutputs{.buffer = return_buffer});
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_Exit_inner()
{
  return {HostFnError{HostFnErrorCode::Exit}, WrappedGasAmount{0}};
}

} // namespace hogd
