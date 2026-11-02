#include "hogd/hog_exec/runtime/syscall_inner.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>

#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
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
    co_return SyscallInnerResult{status.error().to_host_fn_error(),
                                 WrappedGasAmount{0}};
  }
  // Both syscall-visible exits pay the access: the frame walk and item lookup
  // happen before the read can tell whether the key exists or holds the right
  // type, so a miss is not cheaper work than a hit. The load is billed here
  // too -- the key is marked loaded either way, so nothing later recovers the
  // charge and a caller could otherwise probe cold storage for the base cost.
  auto const &[result, load_gas] = *status;
  if (!result.has_value()) {
    co_return SyscallInnerResult{result.error().code(),
                                 WrappedGasAmount{kStorageAccessCost +
                                                  load_gas}};
  }

  auto const bs = *result;

  output_written_len =
      copy_to_guest_buffer(output_bytestring, std::as_bytes(bs));

  // The bytes copied, not the buffer offered: the SDK's zero-alloc read path
  // hands the syscall its whole scratch buffer, so pricing the buffer would
  // charge a 32-byte read out of a 4 KiB scratch for eight blocks.
  const WrappedGasAmount gas{
      GasCost::bytestring_get(std::min(output_bytestring.size(), bs.size())) +
      load_gas};

  if (auto *trace_data =
          syscall_handler.get_syscall_trace_data<SyscallNo::kBytestringGet>()) {
    // The ceiling, not the limit in force: items outlive a lowered limit.
    CHECK(bs.size() <= static_cast<size_t>(kBytestringCeilingLen));
    trace_data->outputs.emplace(SyscallBytestringGetOutputs{
        .bytestring = Bytestring(bs.begin(), bs.end())});
  }

  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone, gas};
};

MaybeAsyncSyscallInnerResult
syscall_BytestringGetLen_inner(SystemCallHandler &syscall_handler,
                               const StorageKeyContractData &input_key,
                               RuntimeMemoryLength &output_length)
{
  auto status = co_await syscall_handler.get_bytestring(input_key);
  if (!status) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(),
                                 WrappedGasAmount{0}};
  }
  // Both syscall-visible exits pay the access; see syscall_BytestringGet_inner.
  auto const &[result, load_gas] = *status;
  if (!result) {
    co_return SyscallInnerResult{result.error().code(),
                                 WrappedGasAmount{kStorageAccessCost +
                                                  load_gas}};
  }

  output_length = result->size();

  if (auto *trace_data =
          syscall_handler
              .get_syscall_trace_data<SyscallNo::kBytestringGetLen>()) {
    trace_data->outputs.emplace(
        SyscallBytestringGetLenOutputs{.length = output_length});
  }

  // A length read copies nothing and is flat in the item's size -- measured
  // flat to within 0.6 ns across three decades -- so it pays the storage
  // access and no block term.
  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               WrappedGasAmount{kStorageAccessCost + load_gas}};
};

MaybeAsyncSyscallInnerResult
syscall_BytestringSet_inner(SystemCallHandler &syscall_handler,
                            WrappedGasAmount remaining_gas,
                            const StorageKeyContractData &input_key,
                            std::span<const std::byte> input_bytestring)
{
  const WrappedGasAmount required_gas{
      GasCost::bytestring_set(input_bytestring.size())};
  // Reject before set_bytestring copies the value, as SHA256 does before
  // hashing; exact exhaustion also faults in the outer wrapper.
  if (remaining_gas.get() <= required_gas.get()) {
    co_return SyscallInnerResult{HostFnError{HostFnErrorCode::OutOfGas},
                                 remaining_gas};
  }
  auto status =
      co_await syscall_handler.set_bytestring(input_key, input_bytestring);
  if (!status) {
    co_return SyscallInnerResult{status.error().to_host_fn_error(),
                                 WrappedGasAmount{0}};
  }
  auto &set_status = *status;
  if (!set_status) {
    co_return SyscallInnerResult{set_status.error().code(),
                                 WrappedGasAmount{0}};
  }

  co_return SyscallInnerResult{SyscallErrorCode::kSyscallErrorCodeNone,
                               required_gas};
}

} // namespace hogd
