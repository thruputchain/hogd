#include "hogd/hogexec/runtime/syscall_inner.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <exception>
#include <span>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/syscall_buffer.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_types.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_BytestringGet_inner(SystemCallHandler &syscall_handler,
                            const StorageKeyContractData &input_key,
                            std::span<std::byte> output_bytestring,
                            RuntimeMemoryLength &output_written_len) noexcept
{
  auto status = syscall_handler.get_bytestring(input_key);
  if (!status.has_value()) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  if (!(*status).has_value()) {
    return {status->error().code(), WrappedGasAmount{0}};
  }

  auto const *bs = (*(*status));

  output_written_len =
      copy_to_guest_buffer(output_bytestring,
                           std::as_bytes(std::span(bs->data(), bs->size())));

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kBytestringGet>()) {
    try {
      CHECK(bs->size() <= kBytestringMaxLen);
      trace_data->outputs.emplace(
          SyscallBytestringGetOutputs{.bytestring = *bs});
    } catch (std::exception &e) {
      report_dropped_syscall_trace(e.what());
      return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
};

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_BytestringGetLen_inner(SystemCallHandler &syscall_handler,
                               const StorageKeyContractData &input_key,
                               RuntimeMemoryLength &output_length) noexcept
{
  auto status = syscall_handler.get_bytestring(input_key);
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  auto &get_status = *status;
  if (!get_status) {
    return {get_status.error().code(), WrappedGasAmount{0}};
  }

  output_length = (*get_status)->size();

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kBytestringGetLen>()) {
    try {
      trace_data->outputs.emplace(
          SyscallBytestringGetLenOutputs{.length = output_length});
    } catch (std::exception &e) {
      report_dropped_syscall_trace(e.what());
      return {HostFnError{HostFnErrorCode::Unrecoverable}, WrappedGasAmount{0}};
    }
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
};

std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_BytestringSet_inner(SystemCallHandler &syscall_handler,
                            const StorageKeyContractData &input_key,
                            std::span<const std::byte> input_bytestring,
                            StorageDeltaFlagSet input_flags) noexcept
{
  if (input_bytestring.size() > kBytestringMaxLen) {
    return {SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }
  auto status =
      syscall_handler.set_bytestring(input_key, input_bytestring, input_flags);
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  auto &set_status = *status;
  if (!set_status) {
    return {set_status.error().code(), WrappedGasAmount{0}};
  }

  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

} // namespace hogd
