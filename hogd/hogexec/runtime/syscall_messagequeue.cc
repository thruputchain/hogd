#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cstddef>
#include <span>
#include <utility>

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "storage_object.hh"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_MessageQueueAppend_inner(SystemCallHandler &syscall_handler,
                                 StorageKey const &key,
                                 std::span<const std::byte> input_message)
{
  auto status = syscall_handler.message_queue_append_event(key, input_message);

  if (!status) {
    return {status.error().code(), 0};
  }

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_MessageQueueConsume_inner(SystemCallHandler &syscall_handler,
                                  StorageKey const &key,
                                  std::span<std::byte> output_message,
                                  RuntimeMemoryLength &output_written_len)
{
  auto status = syscall_handler.message_queue_consume_event(key);

  if (!status) {
    return {status.error().code(), 0};
  }

  size_t write_len = std::min(output_message.size(), status->size());

  std::memcpy(output_message.data(), status->data(), write_len);

  output_written_len = write_len;

  return {SyscallErrorCode::kNone, 0};
}

} // namespace hogd
