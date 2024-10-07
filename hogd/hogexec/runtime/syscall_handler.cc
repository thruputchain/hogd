#include "hogd/hogexec/runtime/syscall_handler.h"

#include <cassert>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_proxy.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "storage_object.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

SystemCallHandler::SystemCallHandler(SignedTransaction signed_tx,
                                     StorageSnapshotCache &snapshot_cache)
  : signed_tx_(std::move(signed_tx)), snapshot_cache_(snapshot_cache)
{
  wasmi_context_.link_fn("hogd", "syscall", &syscall_entry);
}

// syscall methods: can only be called when call_frame_stack_ is nonempty
// (i.e. while at least one invoke() is running)
ContractAddress
SystemCallHandler::get_current_address() const
{
  assert(!call_frame_stack_.empty());

  return call_frame_stack_.back().active_address_;
}

SyscallStatus<ContractAddress>
SystemCallHandler::get_msg_sender() const
{
  assert(!call_frame_stack_.empty());
  if (call_frame_stack_.size() == 1) {
    return SyscallError{SyscallErrorCode::kIsRootTransaction};
  }

  return {call_frame_stack_[call_frame_stack_.size() - 2].active_address_};
}

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
SystemCallHandler::invoke(Invocation invocation)
{
  WasmiRuntime *runtime = resolve_runtime(invocation.invoked_address);

  if (!runtime) {
    return std::pair{SyscallErrorCode::kContractLookupFailed, 0};
  }

  std::string method_name = std::format("{:x}", invocation.method_name);
  uint64_t initial_gas_limit = runtime->get_available_gas();

  call_frame_stack_.emplace_back(invocation.invoked_address, *runtime);

  InvokeResult result = runtime->invoke(method_name, initial_gas_limit);

  // TODO(geoff): an option for the syscall to check the return code
  if (!result.result.has_value()) {
    using enum InvokeErrorCode;

    for (auto const &addr : call_frame_stack_.back().visited_runtimes_) {
      runtimes_.at(addr).unwind_poisoned = true;
    }
    call_frame_stack_.pop_back();

    call_frame_stack_.back().commit_failed_child_call();

    switch (result.result.error().code()) {
    case kUnrecoverableSystemError:
      return {HostFnError{HostFnErrorCode::kUnrecoverableUnknown},
              result.gas_consumed};
    case kDeterministicError:
      // i.e. trap within Wasmi
      return {SyscallErrorCode::kDeterministicWasmError, result.gas_consumed};
    case kOutOfGasError:
      return {HostFnError{HostFnErrorCode::kOutOfGas}, result.gas_consumed};
    default:
      return {HostFnError{HostFnErrorCode::kUnrecoverableUnknown},
              result.gas_consumed};
    }
    // unreachable
  }

  size_t call_frame_stack_size = call_frame_stack_.size();
  if (call_frame_stack_size >= 2) {
    call_frame_stack_[call_frame_stack_size - 2].commit_successful_child_call(
        std::move(call_frame_stack_.back()));
  }
  call_frame_stack_.pop_back();

  // This converts the user-defined return value to a syscall error code.
  // We could do a check here (i.e. reserve < 2^16 for system, everything else
  // for users) but this doesn't particularly matter for correctness.
  return {static_cast<SyscallErrorCode>(*result.result), result.gas_consumed};
}

SyscallStatus<const ReturnBuffer>
SystemCallHandler::get_return_buffer() const
{
  assert(!call_frame_stack_.empty());

  auto const &top_frame = call_frame_stack_.back();

  if (!top_frame.child_return_buffer_.has_value()) {
    return SyscallError{SyscallErrorCode::kNoReturnBufferAvailable};
  }

  return *top_frame.child_return_buffer_;
}

WasmiRuntime *
SystemCallHandler::resolve_runtime(ContractAddress const &addr)
{
  auto it = runtimes_.find(addr);
  if (it != runtimes_.end()) {
    return it->second.runtime.get();
  }

  auto deployed_hash = snapshot_cache_.try_get_deployed_contract_hash(addr);

  if (!deployed_hash) {
    return nullptr;
  }

  auto load_contract = snapshot_cache_.try_get_contract_by_hash(*deployed_hash);

  if (!load_contract) {
    return nullptr;
  }

  std::unique_ptr<WasmiRuntime> new_runtime =
      wasmi_context_.make_new_runtime(load_contract->contract_data, this);

  if (!new_runtime) {
    return nullptr;
  }

  runtimes_.emplace(addr, RuntimeCacheEntry(std::move(new_runtime)));
  return runtimes_[addr].runtime.get();
}

StorageProxy &
SystemCallHandler::resolve_storage_proxy(StorageKey const &key)
{
  AddressAndKey query(get_current_address(), key);

  for (int32_t i = call_frame_stack_.size() - 1; i >= 0; i--) {
    auto *proxy = call_frame_stack_[i].try_get_storage_proxy(query);
    if (proxy) {
      call_frame_stack_.back().push_new_storage_proxy(query, *proxy);
      return *call_frame_stack_.back().try_get_storage_proxy(query);
    }
  }

  call_frame_stack_.back().push_new_storage_proxy(
      query, StorageProxy(snapshot_cache_.try_get_object(query)));
  return *call_frame_stack_.back().try_get_storage_proxy(query);
}

void
SystemCallHandler::delete_last(StorageKey const &key)
{
  auto &proxy = resolve_storage_proxy(key);
  proxy.delete_last();
}

SyscallStatus<StorageObjectType>
SystemCallHandler::get_object_type(StorageKey const &key)
{
  auto &proxy = resolve_storage_proxy(key);
  std::optional<StorageObjectType> result = proxy.get_object_type();

  if (!result) {
    return SyscallError{SyscallErrorCode::kStorageKeyNExist};
  }
  return *result;
}

SyscallStatus<const Bytestring>
SystemCallHandler::get_bytestring(StorageKey const &key)
{
  auto &proxy = resolve_storage_proxy(key);
  return proxy.get_bytestring();
}

SyscallStatus<uint64_t>
SystemCallHandler::get_bytestring_len(StorageKey const &key)
{
  auto &proxy = resolve_storage_proxy(key);
  return proxy.get_bytestring_len();
}

SyscallStatus<void>
SystemCallHandler::set_bytestring(StorageKey const &key,
                                  std::span<const std::byte> input_bytestring)
{
  auto &proxy = resolve_storage_proxy(key);
  return proxy.set_bytestring(input_bytestring);
}

} // namespace hogd
