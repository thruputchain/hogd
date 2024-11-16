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
#include <variant>

#include "auth.hh"
#include "auth_noalloc.hh"
#include "contract.hh"
#include "hogd/base/compiler.h"
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
#include "x23/cleanup.h"
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
    return InvokeResult{.result = InvokeError{InvokeErrorCode::kDeterministic},
                        .gas_consumed = 0};
  }

  if (signed_tx_.tx.invocation.method.method_type() !=
      MethodSpecifierType::kDefaultUint32) {
    return InvokeResult{.result = InvokeError{InvokeErrorCode::kDeterministic},
                        .gas_consumed = 0};
  }

  start_call_frame(signed_tx_.tx.invocation);

  auto res = runtime->runtime_.invoke(signed_tx_.tx.invocation.method,
                                      signed_tx_.tx.invocation.gas_limit);

  finish_call_frame(res);

  if (!res.result) {
    return res;
  }

  // return nonzero error code on root invocation => contract error
  if (*res.result != 0) {
    return InvokeResult{.result = InvokeError{InvokeErrorCode::kDeterministic},
                        .gas_consumed = res.gas_consumed};
  }

  for (auto const &[addr, item] : root_item_cache_map_.local_map_) {
    auto deltas = item.get_deltas();

    if (!deltas.empty()) {
      committed_delta_vec_.emplace_back(addr, deltas);
    }
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
  auto buf = x23::serialize(signed_tx_.tx);
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
    case kUnrecoverable:
      return {HostFnError{HostFnErrorCode::kUnrecoverable},
              result.gas_consumed};
    case kDeterministic:
      // i.e. trap within Wasmi
      return {SyscallErrorCode::kDeterministicWasm, result.gas_consumed};
    case kOutOfGas:
      return {HostFnError{HostFnErrorCode::kOutOfGas}, result.gas_consumed};
    default:
      return {HostFnError{HostFnErrorCode::kUnrecoverable},
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

SyscallStatus<GasAmount>
SystemCallHandler::get_invoke_gas_limit(GasAmount call_specified_gas_limit)
{
  GasAmount initial_gas_limit = UINT64_MAX;
  if (!call_frame_stack_.empty()) {
    initial_gas_limit =
        runtimes_
            .at(call_frame_stack_.back().active_invocation_.invoked_address)
            ->runtime_.get_available_gas();
  }

  // UINT64_MAX as gas_limit implies gas_limit is whatever gas is left
  if (call_specified_gas_limit != UINT64_MAX &&
      call_specified_gas_limit > initial_gas_limit) {
    return SyscallError{SyscallErrorCode::kInsufficientGasForInvocation};
  }

  return std::min<GasAmount>(initial_gas_limit, call_specified_gas_limit);
}

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
SystemCallHandler::invoke(Invocation const &invocation)
{
  auto gas_limit = get_invoke_gas_limit(invocation.gas_limit);

  if (!gas_limit) {
    return std::pair{gas_limit.error().code(), 0};
  }

  auto *runtime = resolve_runtime(invocation.invoked_address);

  if (!runtime) {
    return std::pair{SyscallErrorCode::kContractLookupFailed, 0};
  }

  start_call_frame(invocation);

  auto res = runtime->runtime_.invoke(invocation.method, *gas_limit);

  finish_call_frame(res);

  return convert_invoke_result_to_host_fn_result(res);
}

void
SystemCallHandler::start_call_frame(Invocation const &invocation)
{
  call_frame_stack_.emplace_back(invocation);
}

void
SystemCallHandler::finish_call_frame(InvokeResult const &result)
{
  // Error handling: Each contract's return value is effectively a Result<Value,
  // i32>, or a trap() (InvokeError::kDeterministic).
  //  A result return (through either the return syscall, the
  // method returning, or an Exit syscall) does not revert the function. Only a
  // deterministic trap() (such as out of gas, or divide by 0) cause a contract
  // to be reverted. We do not need a separate Revert() syscall -- a contract
  // can just call trap() or wasm `unreachable`.
  //
  // If a contract has the REVERT_SAFE flag set, then it can be called again
  // after it has been reverted. A contract might not be REVERT_SAFE because
  // reversion does NOT reset a wasm's internal memory or other internal state.
  // We could do that, it's just expensive,
  // and would require fairly close integration with the wasm runtime and some
  // fairly invasive changes (We need to restore the entirety of the wasm
  // module's internal state -- not just memory, but also tables and globals.
  // The stack is reset automatically).

  // Revert the function
  if (!result.result.has_value() || (*result.result < 0)) {

    for (auto const &addr : call_frame_stack_.back().visited_runtimes_) {
      // TODO(geoff) metadata allowing revert_safe declarations
      runtimes_.at(addr)->revert_poisoned_ = true;
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
      commit_last_call_frame_success(call_frame_stack_.back());
    }

    call_frame_stack_.pop_back();
  }
}

bool
SystemCallHandler::current_runtime_is_revert_poisoned() const
{
  return runtimes_
      .at(get_active_call_frame().active_invocation_.invoked_address)
      ->revert_poisoned_;
}

void
SystemCallHandler::commit_last_call_frame_success(CallFrame &frame)
{
  auto logs = frame.extract_logs();

  // Appends to logs, instead of overwrite.
  // Required for the case where verify_auth starts more invocations after the
  // last main invocation finishes
  committed_logs_.insert(committed_logs_.end(), logs.begin(), logs.end());

  root_item_cache_map_.commit_child_map(frame.get_item_cache_map());

  for (auto [addr, hash] : frame.new_contract_deployments_) {
    new_committed_deployments_.emplace(addr, hash);
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

    auto *out = it->second.get();

    if (out->local_deployment_) {
      // the deployment of this contract was reverted
      if (!lookup_locally_deployed_contract(addr)) {
        return nullptr;
      }
    }

    if (out->revert_poisoned_) {
      return nullptr;
    }

    return out;
  }

  bool local_deployment = false;

  auto deployed_hash = snapshot_cache_.try_get_deployed_contract_hash(addr);

  // not found committed deployment; checking local context
  if (!deployed_hash) {
    deployed_hash = lookup_locally_deployed_contract(addr);
    local_deployment = true;
  }

  // not found in local context either
  if (!deployed_hash) {
    return nullptr;
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
  runtimes_.emplace(addr,
                    std::make_unique<RuntimeCacheEntry>(std::move(*new_runtime),
                                                        local_deployment));

  return runtimes_.at(addr).get();
}

ItemCache &
SystemCallHandler::resolve_item_cache(StorageKey const &key)
{
  AddressAndKey query(get_current_address(), key);

  auto &top_frame = get_active_call_frame();

  if (auto *top_frame_item = top_frame.try_get_item_cache(query)) {
    return *top_frame_item;
  }

  for (int32_t i = call_frame_stack_.size() - 2; i >= 0; i--) {
    if (auto *item = call_frame_stack_[i].try_get_item_cache(query)) {
      top_frame.push_new_item_cache(query, *item);
      return *top_frame.try_get_item_cache(query);
    }
  }

  auto root_it = root_item_cache_map_.local_map_.find(query);
  if (root_it != root_item_cache_map_.local_map_.end()) {
    top_frame.push_new_item_cache(query, root_it->second);
    return *top_frame.try_get_item_cache(query);
  }

  top_frame.push_new_item_cache(query, ItemCache(snapshot_cache_.try_get_object(
                                           query)));

  return *top_frame.try_get_item_cache(query);
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

SyscallStatus<void>
SystemCallHandler::replay_cache_insert(StorageKey const &key, uint64_t nonce)
{
  return resolve_item_cache(key).replay_cache_insert(nonce);
}

SyscallStatus<void>
SystemCallHandler::replay_cache_clear(StorageKey const &key, uint64_t threshold)
{
  return resolve_item_cache(key).replay_cache_clear(threshold);
}

SyscallStatus<void>
SystemCallHandler::message_queue_append_event(StorageKey const &key,
                                              std::span<const std::byte> event)
{
  MessageQueueEvent e;

  if (event.size() > kMessageQueueEventMaxLen) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  e.resize(event.size());
  std::memcpy(e.data(), reinterpret_cast<const uint8_t *>(event.data()),
              event.size());

  return resolve_item_cache(key).append_event_to_mq(e);
}

SyscallStatus<MessageQueueEvent>
SystemCallHandler::message_queue_consume_event(StorageKey const &key)
{
  return resolve_item_cache(key).consume_next_mq_event();
}

std::optional<SHA256Hash>
SystemCallHandler::lookup_locally_deployed_contract(
    ContractAddress const &addr) const
{
  for (auto const &frame : call_frame_stack_) {
    auto it = frame.new_contract_deployments_.find(addr);
    if (it != frame.new_contract_deployments_.end()) {
      return it->second;
    }
  }

  auto it = new_committed_deployments_.find(addr);
  if (it != new_committed_deployments_.end()) {
    return it->second;
  }

  return std::nullopt;
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
SystemCallHandler::get_deploy_address(const SHA256Hash *optional_caller_address,
                                      SHA256Hash const &nonce,
                                      SHA256Hash const &contract_hash) const
{
  assert(!call_frame_stack_.empty());

  if (optional_caller_address) {
    return static_get_deploy_address(*optional_caller_address, nonce,
                                     contract_hash);
  }

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

  auto deploy_addr = get_deploy_address(nullptr, nonce, contract_hash);

  // check that we have not already deployed to that address
  if (snapshot_cache_.try_get_deployed_contract_hash(deploy_addr)) {
    return SyscallError{SyscallErrorCode::kContractAlreadyDeployed};
  }

  if (lookup_locally_deployed_contract(deploy_addr).has_value()) {
    return SyscallError{SyscallErrorCode::kContractAlreadyDeployed};
  }

  auto &frame = get_active_call_frame();

  // deploy new contract
  frame.new_contract_deployments_.emplace(deploy_addr, contract_hash);

  return deploy_addr;
}

void
SystemCallHandler::commit_new_contract_deployments(write_client_t &client) const
{
  for (auto const &[addr, hash] : new_committed_deployments_) {

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

uint64_t
SystemCallHandler::get_block_number() const
{
  return snapshot_cache_.get_block_number();
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
