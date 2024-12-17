#include "hogd/hogexec/runtime/syscall_handler.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
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
      std::unordered_map<SHA256Hash, std::shared_ptr<const Contract>> contracts;

      for (auto const &contract : signed_tx_.tx.contracts_to_deploy) {
        auto serialized = x23::serialize(contract);
        contracts.emplace(sha256(std::as_bytes(std::span(serialized))),
                          std::make_shared<const Contract>(contract));
      }
      return contracts;
    }()),
    snapshot_cache_(snapshot_cache),
    invocation_frame_stack_([this](InvocationFrame &last_frame) {
      commit_last_call_frame_success(last_frame);
    })
{
  wasmi_context_.link_fn("marmot", "syscall", &syscall_entry);
}

InvokeResult
SystemCallHandler::root_invoke()
{
  auto *runtime = resolve_runtime(signed_tx_.tx.invocation.invoked_address);

  if (!runtime) {
    return InvokeResult{.result_ = InvokeError{InvokeErrorCode::kDeterministic},
                        .gas_consumed_ = 0};
  }

  invocation_frame_stack_.start_frame(signed_tx_.tx.invocation, runtime);
  auto res = runtime->runtime_.invoke(signed_tx_.tx.invocation.method,
                                      signed_tx_.tx.invocation.gas_limit);
  invocation_frame_stack_.finish_frame(res);

  if (!res.is_success()) {
    if (!res.result_)
      return res;

    // return nonzero error code on root invocation => contract error
    assert(*res.result_ != 0);

    return InvokeResult{.result_ = InvokeError{InvokeErrorCode::kDeterministic},
                        .gas_consumed_ = res.gas_consumed_};
  }

  return res;
}

// can only be called when call_frame_stack_ is nonempty
// (i.e. while at least one invoke() is running)
ContractAddress
SystemCallHandler::get_current_address() const
{
  return invocation_frame_stack_.get_active_frame()
      .active_invocation_.invoked_address;
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
  assert(!invocation_frame_stack_.empty());
  auto res = invocation_frame_stack_.get_msg_sender();
  return res.has_value() ? SyscallStatus<ContractAddress>(*res)
                         : SyscallError{SyscallErrorCode::kIsRootTransaction};
}

SyscallStatus<GasAmount>
SystemCallHandler::get_invoke_gas_limit(GasAmount call_specified_gas_limit)
{
  // UINT64_MAX as gas_limit implies gas_limit is whatever gas is left

  GasAmount initial_gas_limit =
      invocation_frame_stack_.empty()
          ? UINT64_MAX
          : runtimes_
                .at(invocation_frame_stack_.get_active_frame()
                        .active_invocation_.invoked_address)
                ->runtime_.get_available_gas();

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

  invocation_frame_stack_.start_frame(invocation, runtime);
  auto res = runtime->runtime_.invoke(invocation.method, *gas_limit);
  invocation_frame_stack_.finish_frame(res);

  return res.to_host_fn_result();
}

bool
SystemCallHandler::current_runtime_is_revert_poisoned() const
{
  return runtimes_
      .at(invocation_frame_stack_.get_active_frame()
              .active_invocation_.invoked_address)
      ->revert_poisoned_;
}

void
SystemCallHandler::commit_last_call_frame_success(InvocationFrame &frame)
{
  committed_logs_ = frame.extract_logs();
}

SyscallStatus<const ReturnBuffer>
SystemCallHandler::get_return_buffer() const
{
  assert(!invocation_frame_stack_.empty());

  auto const &top_frame = invocation_frame_stack_.get_active_frame();

  return top_frame.child_return_buffer_.has_value()
             ? SyscallStatus<const ReturnBuffer>(
                   *top_frame.child_return_buffer_)
             : SyscallError{SyscallErrorCode::kNoReturnBufferAvailable};
}

void
SystemCallHandler::set_return_buffer(ReturnBuffer &&returned_buffer)
{
  assert(!invocation_frame_stack_.empty());
  invocation_frame_stack_.get_active_frame().set_frame_return(
      std::move(returned_buffer));
}

RuntimeCacheEntry *
SystemCallHandler::resolve_runtime(ContractAddress const &addr)
{
  auto it = runtimes_.find(addr);
  if (it != runtimes_.end()) {
    auto *out = it->second.get();

    // The entry was created by the current transaction.
    // Must check whether that action was rolled back.
    if (out->local_deployment_) {
      if (!lookup_locally_deployed_contract(addr)) {
        // Runtime exists in the cache, but it was deployed in an earlier
        // call frame that has since been reverted.
        return nullptr;
      }
    }

    return out->revert_poisoned_ ? nullptr : out;
  }

  // No runtime cache entry exists; now try to create a new one.

  auto deployed_hash = snapshot_cache_.try_get_deployed_contract_hash(addr);

  bool local_deployment = false;
  if (!deployed_hash) {
    // There was no deployment in the previous snapshot.
    // Now check for the transaction's local context.
    local_deployment = true;
    deployed_hash = lookup_locally_deployed_contract(addr);
  }

  if (!deployed_hash) {
    // A deployment was not found in the local context
    // nor in the snapshot.
    return nullptr;
  }

  // At this point, we have a deployment and the hash of the contract.
  // We now resolve the actual contract code from the snapshot.
  auto load_contract = snapshot_cache_.try_get_contract_by_hash(*deployed_hash);

  if (!load_contract) {
    // Not found committed contract in the snapshot.
    // Try loading from local context
    auto it = newly_created_contracts_.find(*deployed_hash);

    // contract deployment checks to see that a new deployment of a specific
    // hash always is a hash of a contract that the runtime can access.
    assert(it != newly_created_contracts_.end());

    load_contract = it->second;
  }

  // actually make the new runtime
  auto new_runtime =
      wasmi_context_.make_new_runtime(load_contract->contract_data, this);

  // Some error instantiating the runtime.
  // TODO(geoff) validate that failures here are deterministic.
  // (i.e. the result of a malloc() failure).
  if (!new_runtime) {
    return nullptr;
  }

  // successfully resolved runtime
  runtimes_.emplace(addr,
                    std::make_unique<RuntimeCacheEntry>(std::move(*new_runtime),
                                                        local_deployment));

  return runtimes_.at(addr).get();
}

Calldata const &
SystemCallHandler::get_calldata() const
{
  return invocation_frame_stack_.get_active_frame().active_invocation_.calldata;
}

SyscallStatus<void>
SystemCallHandler::log(std::span<const std::byte> message)
{
  assert(!invocation_frame_stack_.empty());

  if (message.size() > kMaxLogMessageLen) {
    // log message too long, return error
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  invocation_frame_stack_.get_active_frame().log(message);

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
  assert(!invocation_frame_stack_.empty());

  // check that contract_hash actually exists
  // This causes a load of the contract to the cache,
  // but this is in most cases no-cost, as a deployed contract will _probably_
  // be immediately invoked with e.g. a constructor
  auto load_contract = snapshot_cache_.try_get_contract_by_hash(contract_hash);

  if (!load_contract) {
    // try loading from local context
    auto it = newly_created_contracts_.find(contract_hash);
    if (it == newly_created_contracts_.end()) {
      // cannot find the contract in question, in either snapshot
      // or in local context.
      return SyscallError{SyscallErrorCode::kContractLookupFailed};
    }
  }

  auto deploy_addr =
      static_get_deploy_address(get_current_address(), nonce, contract_hash);

  if (snapshot_cache_.try_get_deployed_contract_hash(deploy_addr)) {
    // Contract has been deployed to query address in a previous block
    return SyscallError{SyscallErrorCode::kContractAlreadyDeployed};
  }

  if (invocation_frame_stack_.lookup_locally_deployed_contract(deploy_addr)
          .has_value()) {
    // contract has already been deployed in this transaction
    return SyscallError{SyscallErrorCode::kContractAlreadyDeployed};
  }

  auto &frame = invocation_frame_stack_.get_active_frame();

  // deploy new contract
  frame.new_contract_deployments_.emplace(deploy_addr, contract_hash);

  return deploy_addr;
}

} // namespace hogd
