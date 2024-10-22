#include "hogd/hogexec/runtime/syscall_handler.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/call_frame.h"
#include "hogd/hogexec/runtime/item_cache.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "storage_interface.hh"
#include "storage_object.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

SystemCallHandler::SystemCallHandler(SignedTransaction signed_tx,
                                     StorageSnapshotCache &snapshot_cache)
  : signed_tx_(std::move(signed_tx)), newly_created_contracts_([&] {
      std::map<SHA256Hash, std::shared_ptr<const Contract>> contracts;

      for (auto const &contract : signed_tx_.tx.contracts_to_deploy) {
        auto serialized = x23::serialize(contract);
        contracts.emplace(sha256(std::as_bytes(std::span(serialized))),
                          std::make_shared<const Contract>(contract));
      }
      return contracts;
    }()),
    snapshot_cache_(snapshot_cache)
{
  wasmi_context_.link_fn("marmot", "syscall", &syscall_entry);
}

InvokeResult
SystemCallHandler::root_invoke()
{
  auto *runtime = resolve_runtime(signed_tx_.tx.invocation.invoked_address);

  if (!runtime) {
    return InvokeResult{.result =
                            InvokeError{InvokeErrorCode::kDeterministicError},
                        .gas_consumed = 0};
  }

  auto res = invoke_unconverted(signed_tx_.tx.invocation, *runtime,
                                signed_tx_.tx.invocation.gas_limit);

  if (!res.result) {
    return res;
  }

  // return nonzero error code on root invocation => contract error
  if (*res.result != 0) {
    return InvokeResult{.result =
                            InvokeError{InvokeErrorCode::kDeterministicError},
                        .gas_consumed = res.gas_consumed};
  }
  return res;
}

// syscall methods: can only be called when call_frame_stack_ is nonempty
// (i.e. while at least one invoke() is running)
ContractAddress
SystemCallHandler::get_current_address() const
{
  return get_active_call_frame().active_invocation_.invoked_address;
}

SHA256Hash
SystemCallHandler::get_source_tx_hash() const
{
  auto buf = x23::serialize(signed_tx_);
  return sha256(std::as_bytes(std::span(buf)));
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
  uint64_t initial_gas_limit = UINT64_MAX;
  if (!call_frame_stack_.empty()) {
    initial_gas_limit =
        runtimes_
            .at(call_frame_stack_.back().active_invocation_.invoked_address)
            ->runtime.get_available_gas();
  }

  // UINT64_MAX as gas_limit implies gas_limit is whatever gas is left
  if (invocation.gas_limit != UINT64_MAX &&
      invocation.gas_limit > initial_gas_limit) {
    return std::pair{SyscallErrorCode::kInsufficientGasForInvocation, 0};
  }

  const uint64_t gas_limit =
      std::min<uint64_t>(initial_gas_limit, invocation.gas_limit);

  auto *runtime = resolve_runtime(invocation.invoked_address);

  if (!runtime) {
    return std::pair{SyscallErrorCode::kContractLookupFailed, 0};
  }

  return convert_invoke_result_to_host_fn_result(
      invoke_unconverted(invocation, *runtime, gas_limit));
}

void
SystemCallHandler::start_call_frame(Invocation const &invocation)
{
  call_frame_stack_.emplace_back(invocation);
}

void
SystemCallHandler::finish_call_frame(InvokeResult const &result)
{
  // We choose _not_ to check the return code here, and instead pass it back to
  // the caller. Implicitly, this is saying that the caller is responsible for
  // checking the return/exit code, AND that the callee is responsible for
  // cleaning itself up correctly. I.e. if you pass back a return code, you can
  // be safely called again.
  if (!result.result.has_value()) {

    for (auto const &addr : call_frame_stack_.back().visited_runtimes_) {
      // TODO(geoff) metadata allowing unwind_safe declarations
      runtimes_.at(addr)->unwind_poisoned = true;
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
          call_frame_stack_.back());
    }
    else if (call_frame_stack_.size() == 1) {
      commit_last_call_frame(call_frame_stack_.back());
    }

    call_frame_stack_.pop_back();
  }
}

InvokeResult
SystemCallHandler::invoke_unconverted(Invocation const &invocation,
                                      RuntimeCacheEntry &runtime,
                                      uint64_t gas_limit)
{
  start_call_frame(invocation);

  InvokeResult result =
      runtime.runtime.invoke(invocation.method_name, gas_limit);

  finish_call_frame(result);

  return result;
}

void
SystemCallHandler::commit_last_call_frame(CallFrame &frame)
{
  committed_logs_ = frame.extract_logs();

  for (auto const &[addr, proxy] : frame.get_storage_proxies()) {
    auto deltas = proxy.get_deltas();

    if (!deltas.empty()) {
      committed_delta_vec_.emplace_back(addr, deltas);
    }
  }
}

SyscallStatus<const ReturnBuffer>
SystemCallHandler::get_return_buffer() const
{
  auto const &top_frame = get_active_call_frame();

  if (!top_frame.child_return_buffer_.has_value()) {
    return SyscallError{SyscallErrorCode::kNoReturnBufferAvailable};
  }

  return *top_frame.child_return_buffer_;
}

void
SystemCallHandler::set_return_buffer(ReturnBuffer &&returned_buffer)
{
  get_active_call_frame().set_call_return(std::move(returned_buffer));
}

SystemCallHandler::RuntimeCacheEntry *
SystemCallHandler::resolve_runtime(ContractAddress const &addr)
{
  auto it = runtimes_.find(addr);
  if (it != runtimes_.end()) {
    return it->second.get();
  }

  auto deployed_hash = snapshot_cache_.try_get_deployed_contract_hash(addr);

  if (!deployed_hash) {
    // not found committed deployment; checking local context
    auto it = new_deployments_.find(addr);
    if (it == new_deployments_.end()) {
      return nullptr;
    }
    deployed_hash = it->second;
  }

  auto load_contract = snapshot_cache_.try_get_contract_by_hash(*deployed_hash);

  if (!load_contract) {
    // Not found committed contract.
    // Try loading from local context
    auto it = newly_created_contracts_.find(*deployed_hash);
    if (it == newly_created_contracts_.end()) {
      return nullptr;
    }

    load_contract = it->second;
  }

  auto new_runtime =
      wasmi_context_.make_new_runtime(load_contract->contract_data, this);

  if (!new_runtime) {
    return nullptr;
  }

  // successfully resolved runtime
  runtimes_.emplace(addr, std::make_unique<RuntimeCacheEntry>(
                              std::move(*new_runtime)));

  return runtimes_.at(addr).get();
}

ItemCache &
SystemCallHandler::resolve_item_cache(StorageKey const &key)
{
  AddressAndKey query(get_current_address(), key);

  auto &top_frame = get_active_call_frame();

  for (int32_t i = call_frame_stack_.size() - 2; i >= 0; i--) {
    auto *proxy = call_frame_stack_[i].try_get_storage_proxy(query);
    if (proxy) {
      top_frame.push_new_storage_proxy(query, *proxy);
      return *top_frame.try_get_storage_proxy(query);
    }
  }

  auto *proxy = top_frame.try_get_storage_proxy(query);

  if (proxy) {
    return *proxy;
  }

  top_frame.push_new_storage_proxy(query,
                                   ItemCache(
                                       snapshot_cache_.try_get_object(query)));

  return *top_frame.try_get_storage_proxy(query);
}

void
SystemCallHandler::delete_last(StorageKey const &key)
{
  auto &proxy = resolve_item_cache(key);
  proxy.delete_last();
}

SyscallStatus<StorageObjectType>
SystemCallHandler::get_object_type(StorageKey const &key)
{
  auto &proxy = resolve_item_cache(key);
  std::optional<StorageObjectType> result = proxy.get_object_type();

  if (!result) {
    return SyscallError{SyscallErrorCode::kStorageKeyNExist};
  }
  return *result;
}

SyscallStatus<const Bytestring>
SystemCallHandler::get_bytestring(StorageKey const &key)
{
  auto &proxy = resolve_item_cache(key);
  return proxy.get_bytestring();
}

SyscallStatus<RuntimeMemoryLength>
SystemCallHandler::get_bytestring_len(StorageKey const &key)
{
  auto &proxy = resolve_item_cache(key);
  return proxy.get_bytestring_len();
}

SyscallStatus<void>
SystemCallHandler::set_bytestring(StorageKey const &key,
                                  std::span<const std::byte> input_bytestring)
{
  auto &proxy = resolve_item_cache(key);
  return proxy.set_bytestring(input_bytestring);
}

SyscallStatus<const int64_t>
SystemCallHandler::get_nnint64_base_value(StorageKey const &key)
{
  return resolve_item_cache(key).get_nnint64_base_value();
}

SyscallStatus<const int64_t>
SystemCallHandler::get_nnint64_value(StorageKey const &key)
{
  return resolve_item_cache(key).get_nnint64_value();
}

SyscallStatus<void>
SystemCallHandler::nnint64_set_add(StorageKey const &key, int64_t set_value,
                                   int64_t add_value)
{
  return resolve_item_cache(key).nnint64_set_add(set_value, add_value);
}

SyscallStatus<Calldata>
SystemCallHandler::get_calldata() const
{
  return get_active_call_frame().active_invocation_.calldata;
}

SyscallStatus<RuntimeMemoryLength>
SystemCallHandler::get_calldata_len() const
{
  return get_active_call_frame().active_invocation_.calldata.size();
}

SyscallStatus<void>
SystemCallHandler::log(std::span<const std::byte> message)
{
  if (message.size() > kMaxLogMessageLen) {
    return SyscallError{SyscallErrorCode::kStringTooLong};
  }

  get_active_call_frame().log(message);

  return {};
}

RuntimeMemoryLength
SystemCallHandler::get_defaultzero_return_len() const
{
  auto const &top_frame = get_active_call_frame();

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  if (top_frame.child_return_buffer_.has_value()) {
    return top_frame.child_return_buffer_->size();
  }
  // NOLINTEND(bugprone-unchecked-optional-access)
  return 0;
}

ContractAddress
SystemCallHandler::static_get_deploy_address(ContractAddress const &sender,
                                             SHA256Hash const &nonce,
                                             SHA256Hash const &contract_hash)
{
  std::array<uint8_t,
             sizeof(ContractAddress) + sizeof(SHA256Hash) + sizeof(SHA256Hash)>
      hash_buffer;

  std::memcpy(hash_buffer.data(), sender.data(), sizeof(ContractAddress));
  std::memcpy(hash_buffer.data() + sizeof(ContractAddress), nonce.data(),
              sizeof(SHA256Hash));
  std::memcpy(hash_buffer.data() + sizeof(ContractAddress) + sizeof(SHA256Hash),
              contract_hash.data(), sizeof(SHA256Hash));

  return sha256(std::as_bytes(std::span(hash_buffer)));
}

ContractAddress
SystemCallHandler::get_deploy_address(SHA256Hash const &nonce,
                                      SHA256Hash const &contract_hash) const
{
  assert(!call_frame_stack_.empty());

  return static_get_deploy_address(get_current_address(), nonce, contract_hash);
}

SyscallStatus<ContractAddress>
SystemCallHandler::deploy_contract(SHA256Hash const &nonce,
                                   SHA256Hash const &contract_hash)
{
  assert(!call_frame_stack_.empty());

  // check that contract_hash actually exists
  // This causes a load of the contract to the cache,
  // but this is in most cases no-cost, as a deployed contract will _probably_
  // be immediately invoked with e.g. a constructor
  auto load_contract = snapshot_cache_.try_get_contract_by_hash(contract_hash);

  if (!load_contract) {
    // try loading from local context
    auto it = newly_created_contracts_.find(contract_hash);
    if (it == newly_created_contracts_.end()) {
      return SyscallError{SyscallErrorCode::kContractLookupFailed};
    }
  }

  auto deploy_addr = get_deploy_address(nonce, contract_hash);

  // check that we have not already deployed to that address
  if (snapshot_cache_.try_get_deployed_contract_hash(deploy_addr)) {
    return SyscallError{SyscallErrorCode::kContractAlreadyDeployed};
  }

  auto it = new_deployments_.find(deploy_addr);
  if (it != new_deployments_.end()) {
    return SyscallError{SyscallErrorCode::kContractAlreadyDeployed};
  }

  // deploy new contract
  new_deployments_.emplace(deploy_addr, contract_hash);

  return deploy_addr;
}

void
SystemCallHandler::commit_new_contract_deployments(write_client_t &client) const
{
  for (auto const &[addr, hash] : new_deployments_) {

    auto it = newly_created_contracts_.find(hash);
    if (it != newly_created_contracts_.end()) {
      if (client.commit_contract_create(*it->second, SHA256Hash{})() !=
          StorageRPCError::kNone) {
        throw std::runtime_error("unhandled create error");
      }
    }

    if (client.commit_contract_deploy(addr, hash, SHA256Hash{})() !=
        StorageRPCError::kNone) {
      throw std::runtime_error("unhandled deploy error");
    }
  }
}

StorageRPCError
SystemCallHandler::try_reserve_transaction(write_client_t &client) const
{
  return client.try_reserve_transaction_deltas(get_deltas(), SHA256Hash{})();
}

void
SystemCallHandler::commit_transaction(write_client_t &client) const
{
  auto status = client.commit_transaction_deltas(get_deltas(), SHA256Hash{})();

  if (status != StorageRPCError::kNone) {
    throw std::runtime_error("unhandled commit transaction deltas error");
  }

  commit_new_contract_deployments(client);
}

} // namespace hogd
