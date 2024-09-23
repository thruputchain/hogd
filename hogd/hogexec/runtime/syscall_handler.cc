#include "hogd/hogexec/runtime/syscall_handler.h"

#include <cassert>

#include "hogd/hogexec/runtime/call_frame.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "syscall.hh"
#include "transaction.hh"

namespace hogd {

SystemCallHandler::SystemCallHandler(SignedTransaction signed_tx,
                                     StorageSnapshotCache &snapshot_cache)
  : signed_tx_(signed_tx), snapshot_cache_(snapshot_cache)
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

  if (!result.result.has_value()) {
    using enum WasmiInvokeErrorCode;

    for (auto const &addr : call_frame_stack_.back().visited_runtimes_) {
      runtimes_.at(addr).unwind_poisoned = true;
    }
    call_frame_stack_.pop_back();

    switch (result.result.error().code()) {
    case kWasmiFnNExist:
    case kWasmiInputError:
    case kWasmiReturnTypeError:
      // We control the inputs to wasmi.  These types of errors should never
      // occur.
      return {HostFnError{HostFnErrorCode::kUnrecoverableUnknown},
              result.gas_consumed};
    case kWasmiDeterministicCallError:
      // i.e. trap within Wasmi
      return {SyscallErrorCode::kDeterministicWasmError, result.gas_consumed};
    case kWasmiOutOfGasError:
      return {HostFnError{HostFnErrorCode::kOutOfGas}, result.gas_consumed};
    case kWasmiUnrecoverableSystemError:
      return {HostFnError{HostFnErrorCode::kUnrecoverableUnknown},
              result.gas_consumed};
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

  return {SyscallErrorCode::kNone, result.gas_consumed};
}

WasmiRuntime *
SystemCallHandler::resolve_runtime(ContractAddress const &addr)
{
  auto it = runtimes_.find(addr);
  if (it != runtimes_.end()) {
    return it->second.runtime.get();
  }

  auto load_contract = snapshot_cache_.try_get_contract_by_addr(addr);

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

SyscallStatus<const Bytestring>
SystemCallHandler::get_bytestring(StorageKey const &key)
{
  AddressAndKey query(get_current_address(), key);
  auto &proxy = resolve_storage_proxy(key);
  return proxy.get_bytestring();
}

SyscallStatus<uint64_t>
SystemCallHandler::get_bytestring_len(StorageKey const &key)
{
  AddressAndKey query(get_current_address(), key);
  auto &proxy = resolve_storage_proxy(key);
  return proxy.get_bytestring_len();
}

SyscallStatus<void>
SystemCallHandler::set_bytestring(StorageKey const &key,
                                  std::span<const std::byte> input_bytestring)
{
  AddressAndKey query(get_current_address(), key);
  auto &proxy = resolve_storage_proxy(key);
  return proxy.set_bytestring(input_bytestring);
}

} // namespace hogd
