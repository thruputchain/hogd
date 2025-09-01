
#include "hogd/hogexec/runtime/syscall_inner.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_RetrocausalQueueInsert_inner(SystemCallHandler &syscall_handler,
                                     StorageKeyContractData const &key,
                                     std::span<const std::byte> queue_entry)
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
  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
};

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_RetrocausalQueueConsume_inner(SystemCallHandler &syscall_handler,
                                      const StorageKeyContractData &input_key,
                                      bool input_peek,
                                      std::span<std::byte> output_queue_entry,
                                      RuntimeMemoryLength &output_written_len)
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

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
