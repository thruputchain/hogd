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
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/call_frame.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"

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
  // method returning, or an Exit syscall) with a nonnegative error code
  // does not revert the function. Only a
  // deterministic trap() (such as out of gas, or divide by 0),
  // or a negative error code, will cause a contract
  // to be reverted. We do not need a separate Revert() syscall -- a contract
  // can just call trap() or wasm `unreachable`, or Exit(-1).
  //
  // Rationale: The default behavior for a contract can be to propagate
  // unhandled error codes back to the caller.  This makes the revert behavior
  // (revert or not) also propagate in a sensible default way.
  //
  // If a contract has the REVERT_SAFE flag set, then it can be called again
  // after it has been reverted. A contract might not be REVERT_SAFE because
  // reversion does NOT reset a wasm's internal memory or other internal state.
  // We could do that, it's just expensive,
  // and would require fairly close integration with the wasm runtime and some
  // fairly invasive changes (We need to restore the entirety of the wasm
  // module's internal state -- not just memory, but also tables and globals.
  // The wasm stack is reset automatically).

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
  committed_logs_ = frame.extract_logs();
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
      // Runtime exists in the cache, but it was deployed in an earlier
      // call frame that has since been reverted.
      if (!lookup_locally_deployed_contract(addr)) {
        return nullptr;
      }
    }

    if (out->revert_poisoned_) {
      return nullptr;
    }

    return out;
  }

  auto deployed_hash = snapshot_cache_.try_get_deployed_contract_hash(addr);

  bool local_deployment = false;
  // not found committed deployment; checking local context
  if (!deployed_hash) {
    local_deployment = true;
    deployed_hash = lookup_locally_deployed_contract(addr);
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
  return std::nullopt;
}

Calldata const &
SystemCallHandler::get_calldata() const
{
  return get_active_call_frame().active_invocation_.calldata;
}

SyscallStatus<void>
SystemCallHandler::log(std::span<const std::byte> message)
{
  if (message.size() > kMaxLogMessageLen) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  get_active_call_frame().log(message);

  return {};
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

  auto deploy_addr =
      static_get_deploy_address(get_current_address(), nonce, contract_hash);

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

} // namespace hogd
