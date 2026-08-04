
#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <exception>
#include <span>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/syscall_buffer.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_RetrocausalQueueInsert_inner(
    SystemCallHandler &syscall_handler, StorageKeyContractData const &input_key,
    std::span<const std::byte> input_queue_entry) noexcept
{
  auto status =
      syscall_handler.retrocausal_queue_insert(input_key, input_queue_entry);
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  auto &rcq_syscall_status = *status;
  if (!rcq_syscall_status) {
    return {rcq_syscall_status.error().code(), WrappedGasAmount{0}};
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
};

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_RetrocausalQueueConsume_inner(
    SystemCallHandler &syscall_handler, const StorageKeyContractData &input_key,
    bool input_peek, std::span<std::byte> output_queue_entry,
    RuntimeMemoryLength &output_written_len) noexcept
{
  auto status =
      syscall_handler.retrocausal_queue_consume(input_key, input_peek);
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  auto &rcq_syscall_status = *status;
  if (!rcq_syscall_status) {
    return {rcq_syscall_status.error().code(), WrappedGasAmount{0}};
  }

  auto const &queue_entry = *rcq_syscall_status;
  output_written_len =
      copy_to_guest_buffer(output_queue_entry,
                           std::as_bytes(std::span(queue_entry.data(),
                                                   queue_entry.size())));

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kRetrocausalQueueConsume>()) {
    try {
      trace_data->outputs.emplace(
          SyscallRetrocausalQueueConsumeOutputs{.queue_entry = queue_entry});
    } catch (std::exception &e) {
      report_dropped_syscall_trace(e.what());
      return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_RetrocausalQueuePeekLen_inner(SystemCallHandler &syscall_handler,
                                      const StorageKeyContractData &input_key,
                                      RuntimeMemoryLength &output_len) noexcept
{

  auto status =
      syscall_handler.retrocausal_queue_consume(input_key, /*peek=*/true);
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  auto &rcq_syscall_status = *status;
  if (!rcq_syscall_status) {
    return {rcq_syscall_status.error().code(), WrappedGasAmount{0}};
  }

  auto const &queue_entry = *rcq_syscall_status;
  output_len = queue_entry.size();

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kRetrocausalQueuePeekLen>()) {
    try {
      trace_data->outputs.emplace(
          SyscallRetrocausalQueuePeekLenOutputs{.len = output_len});
    } catch (std::exception &e) {
      report_dropped_syscall_trace(e.what());
      return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

} // namespace hogd
