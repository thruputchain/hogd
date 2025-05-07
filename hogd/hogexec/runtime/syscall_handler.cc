#include "hogd/hogexec/runtime/syscall_handler.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// TODO(geoff): Setup configuration parameters for alternative wasm engines
// and/or shared contexts.
SystemCallHandler::SystemCallHandler(
    SignedTransaction signed_tx, StorageSnapshotCache &snapshot_cache,
    std::shared_ptr<RuntimeContext> runtime_context)
  : runtime_context_(std::move(runtime_context)),
    signed_tx_(std::move(signed_tx)),
    invocation_frame_stack_(
        [this](InvocationFrame &last_frame) {
          commit_last_call_frame_success(last_frame);
        },
        snapshot_cache),
    newly_created_contracts_([&] {
      std::unordered_map<SHA256Hash, std::shared_ptr<const Contract>> contracts;

      for (auto const &contract : signed_tx_.tx.contracts_to_deploy) {
        contracts.emplace(sha256(contract),
                          std::make_shared<const Contract>(contract));
      }
      return contracts;
    }()),
    snapshot_cache_(snapshot_cache)
{
  if (!runtime_context_) {
    runtime_context_ = make_hogexec_context(RuntimeEngine::kWasmi);
  }
}

InvokeResult
SystemCallHandler::root_invoke()
{
  auto runtime = resolve_runtime(signed_tx_.tx.invocation.invoked_address);
  if (!runtime) {
    return InvokeResult{.result_ = runtime.error().to_invoke_error(),
                        .gas_consumed_ = 0};
  }
  if (!runtime.value()) {
    return InvokeResult{.result_ =
                            InvokeError{InvokeErrorCode::kInvokeDeterministic},
                        .gas_consumed_ = 0};
  }

  invocation_frame_stack_.start_frame(signed_tx_.tx.invocation);
  auto res = runtime.value()->invoke(signed_tx_.tx.invocation.method,
                                     signed_tx_.tx.invocation.gas_limit);
  invocation_frame_stack_.finish_frame(res);

  if (!res.is_root_success())
    return res.to_root_error();
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
  return sha256(signed_tx_.tx);
}

SyscallStatus<ContractAddress>
SystemCallHandler::get_msg_sender() const
{
  assert(!invocation_frame_stack_.empty());
  auto res = invocation_frame_stack_.get_msg_sender();
  return res.has_value() ? SyscallStatus<ContractAddress>(*res)
                         : SyscallError{SyscallErrorCode::kIsRootTransaction};
}

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
SystemCallHandler::invoke(Invocation const &invocation, GasAmount gas_limit)
{
  auto runtime = resolve_runtime(invocation.invoked_address);
  if (!runtime) {
    return std::pair{runtime.error().to_host_fn_error(), 0};
  }
  if (!runtime.value()) {
    return std::pair{SyscallErrorCode::kContractLookupFailed, 0};
  }

  invocation_frame_stack_.start_frame(invocation);
  auto res = runtime.value()->invoke(invocation.method, gas_limit);
  invocation_frame_stack_.finish_frame(res);

  return res.to_host_fn_result();
}

void
SystemCallHandler::commit_last_call_frame_success(InvocationFrame &frame)
{
  committed_logs_ = frame.extract_logs();
}

SyscallStatus<const ReturnBuffer *>
SystemCallHandler::get_return_buffer() const
{
  assert(!invocation_frame_stack_.empty());
  auto const &top_frame = invocation_frame_stack_.get_active_frame();

  return top_frame.child_return_buffer_.has_value()
             ? SyscallStatus<const ReturnBuffer *>(
                   &(*top_frame.child_return_buffer_))
             : SyscallError{SyscallErrorCode::kNoReturnBufferAvailable};
}

void
SystemCallHandler::set_return_buffer(ReturnBuffer &&returned_buffer)
{
  assert(!invocation_frame_stack_.empty());
  invocation_frame_stack_.get_active_frame().set_frame_return(
      std::move(returned_buffer));
}

SystemStatus<std::unique_ptr<Runtime>>
SystemCallHandler::resolve_runtime(ContractAddress const &addr)
{
  // check if the address has a deployed contract hash
  auto deployed_hash_expect =
      snapshot_cache_.try_get_deployed_contract_hash(addr);
  if (!deployed_hash_expect) {
    return deployed_hash_expect.error();
  }
  auto deployed_hash = *deployed_hash_expect;

  if (!deployed_hash) {
    // There was no deployment in the previous snapshot.
    // Now check for the transaction's local context.
    deployed_hash =
        invocation_frame_stack_.lookup_locally_deployed_contract(addr);
  }

  if (!deployed_hash) {
    // A deployment was not found in the local context
    // nor in the snapshot.
    return nullptr;
  }

  // At this point, we have a deployment and the hash of the contract.
  // We now resolve the actual contract code from the snapshot.
  auto load_contract_expect =
      snapshot_cache_.try_get_contract_by_hash(*deployed_hash);
  if (!load_contract_expect) {
    return load_contract_expect.error();
  }
  auto &load_contract = *load_contract_expect;

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
  // TODO(geoff): do verification when contracts are created, not each time.
  assert(load_contract);
  return runtime_context_->make_new_runtime(*load_contract, this, true);
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
  return sha256(ContractDeploymentHashPreimage{.deployer_address = sender,
                                               .nonce = nonce,
                                               .contract_hash = contract_hash});
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

SystemStatus<SyscallStatus<std::span<const std::byte>>>
SystemCallHandler::get_bytestring(StorageKey const &storage_key)
{
  auto item_view = invocation_frame_stack_.get_item_view(storage_key);
  if (!item_view) {
    return item_view.error();
  }
  assert(*item_view);

  if (auto const *bytestring_item = (*item_view)->get<BytestringItem>()) {
    return std::as_bytes(std::span(bytestring_item->bytestring_));
  }

  if (auto const *unmodified_item = (*item_view)->get<UnmodifiedItem>()) {
    if (unmodified_item->base_item_.type() ==
        StorageItemType::kItemTypeDeleted) {
      return SyscallError{SyscallErrorCode::kStorageKeyNotExist};
    }
    if (unmodified_item->base_item_.type() !=
        StorageItemType::kItemTypeBytestring) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    return std::as_bytes(std::span(unmodified_item->base_item_.bytestring()));
  }

  return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_bytestring(StorageKey const &storage_key,
                                  std::span<const std::byte> input_bytestring)
{
  auto item_view = invocation_frame_stack_.get_item_view(storage_key);
  if (!item_view) {
    return item_view.error();
  }
  assert(*item_view);

  Bytestring new_string;
  new_string.resize_uninitialized(input_bytestring.size());
  std::memcpy(new_string.data(),
              reinterpret_cast<const uint8_t *>(input_bytestring.data()),
              input_bytestring.size());

  return (*item_view)->set(BytestringItem(std::move(new_string)));
}

SystemStatus<SyscallStatus<std::pair<int128_t, int128_t>>>
SystemCallHandler::get_nnint(StorageKey const &storage_key)
{
  auto item_view = invocation_frame_stack_.get_item_view(storage_key);
  if (!item_view) {
    return item_view.error();
  }
  assert(*item_view);

  if (auto const *nnint_item = (*item_view)->get<NNIntItem>()) {
    return {std::make_pair(nnint_item->base_value_, nnint_item->add_value_)};
  }

  if (auto const *unmodified_item = (*item_view)->get<UnmodifiedItem>()) {
    if (unmodified_item->base_item_.type() ==
        StorageItemType::kItemTypeDeleted) {
      return SyscallError{SyscallErrorCode::kStorageKeyNotExist};
    }
    if (unmodified_item->base_item_.type() !=
        StorageItemType::kItemTypeNonnegativeInt) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    return {std::make_pair(unmodified_item->base_item_.nnint(), 0)};
  }

  return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_nnint(StorageKey const &storage_key, int128_t set_value,
                             int128_t add_value)
{
  auto item_view = invocation_frame_stack_.get_item_view(storage_key);
  if (!item_view) {
    return item_view.error();
  }
  assert(*item_view);

  return (*item_view)
      ->set(NNIntItem{.base_value_ = set_value, .add_value_ = add_value});
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::delete_object(StorageKey const &storage_key)
{
  auto item_view = invocation_frame_stack_.get_item_view(storage_key);
  if (!item_view) {
    return item_view.error();
  }
  assert(*item_view);

  return (*item_view)->set(DeletedItem{});
}

} // namespace hogd
