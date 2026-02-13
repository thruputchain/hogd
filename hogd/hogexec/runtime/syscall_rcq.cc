
#include "hogd/hogexec/runtime/syscall_inner.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <span>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_RetrocausalQueueInsert_inner(
    SystemCallHandler &syscall_handler, StorageKeyContractData const &key,
    std::span<const std::byte> queue_entry) noexcept
{
  auto status = syscall_handler.retrocausal_queue_insert(key, queue_entry);
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  auto &rcq_syscall_status = *status;
  if (!rcq_syscall_status) {
    return {rcq_syscall_status.error().code(), 0};
  }

  // insertion has been successful
  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<
              SyscallNo::kRetrocausalQueueInsert>()) {
    try {
      trace_data->success().key = key;
      auto const *data =
          reinterpret_cast<const uint8_t *>(queue_entry.data());
      trace_data->success().queue_entry.append(data,
                                               data + queue_entry.size());
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}",
            SyscallNo::kRetrocausalQueueInsert, e.what());
    }
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
};

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_RetrocausalQueueConsume_inner(
    SystemCallHandler &syscall_handler, const StorageKeyContractData &input_key,
    bool input_peek, std::span<std::byte> output_queue_entry,
    RuntimeMemoryLength &output_written_len) noexcept
{
  auto status =
      syscall_handler.retrocausal_queue_consume(input_key, input_peek);
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  auto &rcq_syscall_status = *status;
  if (!rcq_syscall_status) {
    return {rcq_syscall_status.error().code(), 0};
  }

  auto const &queue_entry = *rcq_syscall_status;
  output_written_len = std::min<RuntimeMemoryLength>(output_queue_entry.size(),
                                                     queue_entry.size());
  std::memcpy(output_queue_entry.data(), queue_entry.data(),
              output_written_len);

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<
              SyscallNo::kRetrocausalQueueConsume>()) {
    try {
      trace_data->success().key = input_key;
      trace_data->success().peek = input_peek;
      trace_data->success().queue_entry.append(queue_entry.data(),
                                               queue_entry.data() +
                                                   queue_entry.size());
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}",
            SyscallNo::kRetrocausalQueueConsume, e.what());
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
