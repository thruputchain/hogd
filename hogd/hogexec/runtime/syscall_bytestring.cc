#include "hogd/hogexec/runtime/syscall_inner.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <span>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringGet_inner(SystemCallHandler &syscall_handler,
                            const StorageKeyContractData &input_key,
                            std::span<std::byte> output_bytestring,
                            RuntimeMemoryLength &output_written_len) noexcept
{
  auto status = syscall_handler.get_bytestring(input_key);
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }

  auto const *bs = status.value().value();

  output_written_len =
      std::min<RuntimeMemoryLength>(output_bytestring.size(), bs->size());

  if (output_written_len > 0) {
    std::memcpy(reinterpret_cast<uint8_t *>(output_bytestring.data()),
                bs->data(), output_written_len);
  }

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kBytestringGet>()) {
    try {
      CHECK(bs->size() <= kBytestringMaxLen);
      trace_data->outputs.emplace(
          SyscallBytestringGetOutputs{.bytestring = *bs});
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kBytestringGet,
            e.what());
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
};

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringGetLen_inner(SystemCallHandler &syscall_handler,
                               const StorageKeyContractData &input_key,
                               RuntimeMemoryLength &output_length) noexcept
{
  auto status = syscall_handler.get_bytestring(input_key);
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }

  output_length = (*(*status))->size();

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kBytestringGetLen>()) {
    try {
      trace_data->outputs.emplace(
          SyscallBytestringGetLenOutputs{.length = output_length});
    } catch (std::exception &e) {
      panic("Failed to trace syscall {}: {}", SyscallNo::kBytestringGetLen,
            e.what());
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
};

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
syscall_BytestringSet_inner(SystemCallHandler &syscall_handler,
                            const StorageKeyContractData &input_key,
                            std::span<const std::byte> input_bytestring,
                            StorageDeltaFlagSet input_flags) noexcept
{
  if (input_bytestring.size() > kBytestringMaxLen) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0};
  }
  auto status =
      syscall_handler.set_bytestring(input_key, input_bytestring, input_flags);
  if (!status) {
    return {status.error().to_host_fn_error(), 0};
  }
  if (!status.value()) {
    return {status.value().error().code(), 0};
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, 0};
}

} // namespace hogd
