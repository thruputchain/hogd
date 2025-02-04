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

std::pair<HostFnResult, GasAmount>
syscall_Return_inner(SystemCallHandler &syscall_handler,
                     std::span<const std::byte> input_buffer)
{
  if (input_buffer.size() > kMaxReturnBufferLen) {
    return {{HostFnErrorCode::kExit,
             std::to_underlying(SyscallErrorCode::kDeterministicWasm)},
            0};
  }

  ReturnBuffer buffer;
  buffer.resize_uninitialized(input_buffer.size());
  std::memcpy(buffer.data(),
              reinterpret_cast<const uint8_t *>(input_buffer.data()),
              input_buffer.size());
  syscall_handler.set_return_buffer(std::move(buffer));

  return {{HostFnErrorCode::kReturn,
           std::to_underlying(SyscallErrorCode::kNone)},
          0};
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

  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_GetReturnedBufferLen_inner(SystemCallHandler &syscall_handler,
                                   RuntimeMemoryLength &output_buffer_len)
{
  auto status = syscall_handler.get_return_buffer();
  if (!status) {
    return {status.error().code(), 0};
  }

  output_buffer_len = (*status)->size();
  return {SyscallErrorCode::kNone, 0};
}

std::pair<HostFnResult, GasAmount>
syscall_Exit_inner(const int32_t input_code)
{
  return {{HostFnErrorCode::kExit, input_code}, 0};
}

} // namespace hogd
