#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/syscall_buffer.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_types.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction_trace.hh"
#include "types.hh"

namespace hogd {

MaybeAsyncSyscallInnerResult
syscall_BytestringGet_inner(SystemCallHandler &syscall_handler,
                            const StorageKeyContractData &input_key,
                            std::span<std::byte> output_bytestring,
                            RuntimeMemoryLength &output_written_len)
{
  auto status = co_await syscall_handler.get_bytestring(input_key);
  if (!status.has_value()) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  if (!(*status).has_value()) {
    co_return SyscallInnerResult{status->error().code(), WrappedGasAmount{0}};
  }

  auto const *bs = (*(*status));

  output_written_len =
      copy_to_guest_buffer(output_bytestring,
                           std::as_bytes(std::span(bs->data(), bs->size())));

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kBytestringGet>()) {
    CHECK(bs->size() <= kBytestringMaxLen);
    trace_data->outputs.emplace(SyscallBytestringGetOutputs{.bytestring = *bs});
  }

  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
};

MaybeAsyncSyscallInnerResult
syscall_BytestringGetLen_inner(SystemCallHandler &syscall_handler,
                               const StorageKeyContractData &input_key,
                               RuntimeMemoryLength &output_length)
{
  auto status = co_await syscall_handler.get_bytestring(input_key);
  if (!status) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  auto &get_status = *status;
  if (!get_status) {
    co_return SyscallInnerResult{get_status.error().code(), WrappedGasAmount{0}};
  }

  output_length = (*get_status)->size();

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kBytestringGetLen>()) {
    trace_data->outputs.emplace(
        SyscallBytestringGetLenOutputs{.length = output_length});
  }

  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
};

MaybeAsyncSyscallInnerResult
syscall_BytestringSet_inner(SystemCallHandler &syscall_handler,
                            const StorageKeyContractData &input_key,
                            std::span<const std::byte> input_bytestring,
                            StorageDeltaFlagSet input_flags)
{
  if (input_bytestring.size() > kBytestringMaxLen) {
    co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput,
            WrappedGasAmount{0}};
  }
  auto status = co_await
      syscall_handler.set_bytestring(input_key, input_bytestring, input_flags);
  if (!status) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  auto &set_status = *status;
  if (!set_status) {
    co_return SyscallInnerResult{set_status.error().code(), WrappedGasAmount{0}};
  }

  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}

} // namespace hogd
