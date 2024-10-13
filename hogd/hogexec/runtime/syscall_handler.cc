#include "hogd/hogexec/runtime/syscall_handler.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/format.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_proxy.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "storage_interface.hh"
#include "storage_object.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

SystemCallHandler::SystemCallHandler(SignedTransaction signed_tx,
                                     StorageSnapshotCache &snapshot_cache)
  : signed_tx_(std::move(signed_tx)), snapshot_cache_(snapshot_cache)
{
  wasmi_context_.link_fn("marmot", "syscall", &syscall_entry);
}

InvokeResult
SystemCallHandler::root_invoke()
{
  WasmiRuntime *runtime =
      resolve_runtime(signed_tx_.tx.invocation.invoked_address);

  if (!runtime) {
    return InvokeResult{.result =
                            InvokeError{InvokeErrorCode::kDeterministicError},
                        .gas_consumed = 0};
  }

  return invoke_unconverted(signed_tx_.tx.invocation, runtime,
                            signed_tx_.tx.invocation.gas_limit);
}

// syscall methods: can only be called when call_frame_stack_ is nonempty
// (i.e. while at least one invoke() is running)
ContractAddress
SystemCallHandler::get_current_address() const
{
  assert(!call_frame_stack_.empty());

  return call_frame_stack_.back().active_invocation_.invoked_address;
}

SyscallStatus<ContractAddress>
SystemCallHandler::get_msg_sender() const
{
  assert(!call_frame_stack_.empty());
  if (call_frame_stack_.size() == 1) {
    return SyscallError{SyscallErrorCode::kIsRootTransaction};
  }

  return {call_frame_stack_[call_frame_stack_.size() - 2]
              .active_invocation_.invoked_address};
}

namespace {

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
convert_invoke_result_to_host_fn_result(InvokeResult const &result)
{
  if (!result.result.has_value()) {
    using enum InvokeErrorCode;

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

  // This converts the user-defined return value to a syscall error code.
  // We could do a check here (i.e. reserve < 2^16 for system, everything else
  // for users) but this doesn't particularly matter for correctness.
  return {static_cast<SyscallErrorCode>(*result.result), result.gas_consumed};
}

} // namespace

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
SystemCallHandler::invoke(Invocation const &invocation)
{
  auto *runtime = resolve_runtime(invocation.invoked_address);

  if (!runtime) {
    return std::pair{SyscallErrorCode::kContractLookupFailed, 0};
  }

  uint64_t initial_gas_limit = UINT64_MAX;
  if (!call_frame_stack_.empty()) {
    initial_gas_limit = call_frame_stack_.back().get_available_gas();
  }

  // UINT64_MAX as gas_limit implies gas_limit is whatever gas is left
  if (invocation.gas_limit != UINT64_MAX &&
      invocation.gas_limit > initial_gas_limit) {
    return std::pair{SyscallErrorCode::kInsufficientGasForInvocation, 0};
  }

  const uint64_t gas_limit =
      std::min<uint64_t>(initial_gas_limit, invocation.gas_limit);

  auto invoke_result = invoke_unconverted(invocation, runtime, gas_limit);

  return convert_invoke_result_to_host_fn_result(invoke_result);
}

InvokeResult
SystemCallHandler::invoke_unconverted(Invocation const &invocation,
                                      WasmiRuntime *runtime, uint64_t gas_limit)
{
  call_frame_stack_.emplace_back(invocation, *runtime);

  auto method_name = std::format("{:x}", invocation.method_name);

  InvokeResult result = runtime->invoke(method_name, gas_limit);

  // We choose _not_ to check the return code here, and instead pass it back to
  // the caller. Implicitly, this is saying that the caller is responsible for
  // checking the return code, AND that the callee is responsible for cleaning
  // itself up correctly. I.e. if you pass back a return code, you can be safely
  // called again.
  if (!result.result.has_value()) {

    for (auto const &addr : call_frame_stack_.back().visited_runtimes_) {
      // TODO(geoff) metadata allowing unwind_safe declarations
      runtimes_.at(addr).unwind_poisoned = true;
    }

    call_frame_stack_.pop_back();

    if (!call_frame_stack_.empty()) {
      call_frame_stack_.back().commit_failed_child_call();
    }
  }
  else {
    size_t call_frame_stack_size = call_frame_stack_.size();

    if (call_frame_stack_size >= 2) {
      call_frame_stack_[call_frame_stack_size - 2].commit_successful_child_call(
          std::move(call_frame_stack_.back()));
    }
    else if (call_frame_stack_.size() == 1) {
      commit_last_call_frame(std::move(call_frame_stack_.back()));
    }

    call_frame_stack_.pop_back();
  }

  return result;
}

void
SystemCallHandler::commit_last_call_frame(CallFrame &&frame)
{
  committed_logs_ = frame.extract_logs();

  for (auto const &[addr, proxy] : frame.get_storage_proxies()) {
    auto deltas = proxy->get_deltas();

    if (!deltas.empty()) {
      committed_delta_vec_.emplace_back(addr, deltas);
    }
  }
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

void
SystemCallHandler::set_return_buffer(ReturnBuffer &&returned_buffer)
{
  assert(!call_frame_stack_.empty());

  call_frame_stack_.back().set_call_return(std::move(returned_buffer));
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

  for (int32_t i = call_frame_stack_.size() - 2; i >= 0; i--) {
    auto *proxy = call_frame_stack_[i].try_get_storage_proxy(query);
    if (proxy) {
      call_frame_stack_.back().push_new_storage_proxy(query, *proxy);
      return *call_frame_stack_.back().try_get_storage_proxy(query);
    }
  }

  auto *proxy = call_frame_stack_.back().try_get_storage_proxy(query);

  if (proxy) {
    return *proxy;
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

SyscallStatus<RuntimeMemoryLength>
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

SyscallStatus<const int64_t>
SystemCallHandler::get_nnint64_base_value(StorageKey const &key)
{
  return resolve_storage_proxy(key).get_nnint64_base_value();
}

SyscallStatus<const int64_t>
SystemCallHandler::get_nnint64_value(StorageKey const &key)
{
  return resolve_storage_proxy(key).get_nnint64_value();
}

SyscallStatus<void>
SystemCallHandler::nnint64_set_add(StorageKey const &key, int64_t set_value,
                                   int64_t add_value)
{
  return resolve_storage_proxy(key).nnint64_set_add(set_value, add_value);
}

SyscallStatus<Calldata>
SystemCallHandler::get_calldata() const
{
  assert(!call_frame_stack_.empty());

  return call_frame_stack_.back().active_invocation_.calldata;
}

SyscallStatus<RuntimeMemoryLength>
SystemCallHandler::get_calldata_len() const
{
  assert(!call_frame_stack_.empty());
  return call_frame_stack_.back().active_invocation_.calldata.size();
}

SyscallStatus<void>
SystemCallHandler::log(std::span<const std::byte> message)
{
  if (message.size() > kMaxLogMessageLen) {
    return SyscallError{SyscallErrorCode::kStringTooLong};
  }

  assert(!call_frame_stack_.empty());

  call_frame_stack_.back().log(message);

  return {};
}

RuntimeMemoryLength
SystemCallHandler::get_defaultzero_return_len() const
{
  assert(!call_frame_stack_.empty());

  if (call_frame_stack_.back().child_return_buffer_.has_value()) {
    return call_frame_stack_.back().child_return_buffer_->size();
  }
  return 0;
}

} // namespace hogd
