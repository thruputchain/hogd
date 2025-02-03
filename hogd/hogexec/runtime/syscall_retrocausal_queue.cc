#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <utility>

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_RetrocausalQueueInsert_inner(SystemCallHandler &syscall_handler,
                                     StorageKey const &key,
                                     std::span<const std::byte> queue_entry)
{
  auto status = syscall_handler.retrocausal_queue_insert(key, queue_entry);
  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
};

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_RetrocausalQueueConsume_inner(SystemCallHandler &syscall_handler,
                                      const StorageKey &input_key,
                                      bool input_peek,
                                      std::span<std::byte> output_queue_entry,
                                      RuntimeMemoryLength &output_written_len)
{
  auto status =
      syscall_handler.retrocausal_queue_consume(input_key, input_peek);
  if (!status) {
    return {HostFnError{status.error()}, 0};
  }

  auto &rcq_status = *status;
  if (!rcq_status) {
    return {rcq_status.error().code(), 0};
  }

  auto const &queue_entry = *rcq_status;
  output_written_len = std::min<RuntimeMemoryLength>(output_queue_entry.size(),
                                                     queue_entry.size());
  std::memcpy(output_queue_entry.data(), queue_entry.data(),
              output_written_len);

  std::printf("got queue entry from rcq consume: %s\n",
              x23::show(queue_entry).c_str());
  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
