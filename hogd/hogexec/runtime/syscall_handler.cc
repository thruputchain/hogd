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
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

// TODO(geoff): Setup configuration parameters for alternative wasm engines
// and/or shared contexts.
SystemCallHandler::SystemCallHandler(
    SignedTransaction signed_tx, StorageSnapshotCache &snapshot_cache,
    PerTransactionDatabaseProxy db_proxy, ExecutionModeData execution_mode_data,
    std::shared_ptr<RuntimeContext> runtime_context)
  : runtime_context_(std::move(runtime_context)),
    signed_tx_(std::move(signed_tx)), db_proxy_(std::move(db_proxy)),
    invocation_frame_stack_(
        [this](InvocationFrame &&last_frame) {
          commit_last_call_frame_success(std::move(last_frame));
        },
        snapshot_cache, db_proxy_),
    newly_created_contracts_([&] {
      std::unordered_map<SHA256Hash, std::shared_ptr<const Contract>> contracts;

      for (auto const &contract : signed_tx_.tx.contracts_to_deploy) {
        contracts.emplace(sha256(contract),
                          std::make_shared<const Contract>(contract));
      }
      return contracts;
    }()),
    execution_mode_data_(std::move(execution_mode_data)),
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

  auto start_status =
      invocation_frame_stack_.start_frame(signed_tx_.tx.invocation,
                                          std::to_underlying(
                                              InvokeFlag::kInvokeFlagNone));
  if (!start_status.has_value()) {
    return InvokeResult{.result_ = start_status.error().to_invoke_error(),
                        .gas_consumed_ = 0};
  }

  auto res = runtime.value()->invoke(signed_tx_.tx.invocation.method,
                                     signed_tx_.tx.invocation.gas_limit);
  auto finish_status =
      invocation_frame_stack_.finish_frame(res, execution_mode_data_);
  if (!finish_status.has_value()) {
    return InvokeResult{.result_ = finish_status.error().to_invoke_error(),
                        .gas_consumed_ = 0};
  }
  res = finish_status.value();

  if (!res.is_root_success())
    return res.to_root_error();
  root_invoked_succeeded_ = true;
  return res;
}

x23::Async<SystemStatus<bool>>
SystemCallHandler::try_commit_transaction()
{
  assert(!transaction_committed_);
  if (!root_invoked_succeeded_) {

    auto st = co_await db_proxy_.abort_transaction();
    if (!st.has_value()) {
      co_return st.error();
    }

    co_return false;
  }
  assert(last_frame_.has_value());

  auto r = co_await last_frame_.value().try_acquire_frame_locks(db_proxy_);
  if (!r.has_value()) {
    co_return r.error();
  }

  bool result = r.value();

  // check if any of the 2pc database operations failed
  if (result) {
    auto res = co_await db_proxy_.commit_transaction();
    if (!res.has_value()) {
      co_return res.error();
    }
    transaction_committed_ = true;
    co_return true;
  }

  auto res = co_await db_proxy_.abort_transaction();
  if (!res.has_value()) {
    co_return res.error();
  }
  transaction_committed_ = true;
  co_return false;
}

// can only be called when call_frame_stack_ is nonempty
// (i.e. while at least one invoke() is running)
ContractAddress
SystemCallHandler::get_current_address() const
{
  return invocation_frame_stack_.get_active_frame()
      .active_invocation()
      .invoked_address;
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
SystemCallHandler::invoke(Invocation const &invocation, GasAmount gas_limit,
                          InvokeFlagSet flags)
{
  auto runtime = resolve_runtime(invocation.invoked_address);
  if (!runtime) {
    return std::pair{runtime.error().to_host_fn_error(), 0};
  }
  if (!runtime.value()) {
    return std::pair{SyscallErrorCode::kContractLookupFailed, 0};
  }

  auto start_status = invocation_frame_stack_.start_frame(invocation, flags);
  if (!start_status.has_value()) {
    return {start_status.error().to_host_fn_error(), 0};
  }
  auto res = runtime.value()->invoke(invocation.method, gas_limit);
  auto finish_status =
      invocation_frame_stack_.finish_frame(res, execution_mode_data_);
  if (!finish_status.has_value()) {
    return {finish_status.error().to_host_fn_error(), 0};
  }
  res = finish_status.value();

  return res.to_host_fn_result();
}

void
SystemCallHandler::commit_last_call_frame_success(InvocationFrame &&frame)
{
  last_frame_.emplace(std::move(frame));
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
  return invocation_frame_stack_.get_active_frame()
      .active_invocation()
      .calldata;
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

SystemStatus<SyscallStatus<ContractAddress>>
SystemCallHandler::deploy_contract(SHA256Hash const &nonce,
                                   SHA256Hash const &contract_hash)
{
  assert(!invocation_frame_stack_.empty());

  // check that contract_hash actually exists
  // This causes a load of the contract to the cache,
  // but this is in most cases no-cost, as a deployed contract will _probably_
  // be immediately invoked with e.g. a constructor
  auto load_contract_status =
      snapshot_cache_.try_get_contract_by_hash(contract_hash);
  if (!load_contract_status) {
    return load_contract_status.error();
  }
  auto &load_contract = *load_contract_status;

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

  auto deploy_check_status =
      snapshot_cache_.try_get_deployed_contract_hash(deploy_addr);
  if (!deploy_check_status) {
    return deploy_check_status.error();
  }
  if (*deploy_check_status) {
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

namespace {
auto queue_entry_from_span(std::span<const std::byte> span) {
  QueueEntry new_entry;
  new_entry.resize_uninitialized(span.size());
  std::memcpy(new_entry.data(),
              reinterpret_cast<const uint8_t *>(span.data()),
              span.size());
  return new_entry;
}

SyscallStatus<RetrocausalQueueItem*>
retrocausal_queue_item_from_view(ItemView* item_view) {
  if (auto* queue_item = item_view->get<RetrocausalQueueItem>()) {
    return queue_item;
  }
  if (auto const* unmodified_item = item_view->get<UnmodifiedItem>()) {
    RetrocausalQueueItem new_item(unmodified_item->base_item_);

    auto set_status = item_view->set(new_item);
    if (!set_status.has_value()) {
      return set_status.error();
    }
    return item_view->get<RetrocausalQueueItem>();
  }
  return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
}
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::retrocausal_queue_insert(StorageKey const &key,
                                            std::span<const std::byte> queue_entry)
{
  auto item_view = invocation_frame_stack_.get_item_view(key);
  if (!item_view) {
    return item_view.error();
  }
  assert(*item_view);

  auto queue_item = retrocausal_queue_item_from_view(*item_view);
  if (!queue_item.has_value()) {
    return queue_item.error();
  }

  // Now we have either grabbed the preexisting RCQItem, or created a new one.
  // In either case, we can now insert the new entry.
  queue_item.value()->insert_entry(queue_entry_from_span(queue_entry));
  return {};
}

SystemStatus<SyscallStatus<QueueEntry>>
SystemCallHandler::retrocausal_queue_consume(StorageKey const &key,
                                             bool peek)
{
  auto item_view = invocation_frame_stack_.get_item_view(key);
  if (!item_view) {
    return item_view.error();
  }
  assert(*item_view);

  auto queue_item = retrocausal_queue_item_from_view(*item_view);
  if (!queue_item.has_value()) {
    return queue_item.error();
  }

  auto entry = queue_item.value()->get_next_item(execution_mode_data_, peek);
  if (!entry.has_value()) {
    return entry.error();
  }

  auto& value = entry.value();
  if (value.has_value()) {
    return value.value();
  }
  return SyscallError{SyscallErrorCode::kNoRetrocausalQueueEntry};
}



} // namespace hogd
