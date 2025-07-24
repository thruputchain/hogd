#include "hogd/hogexec/runtime/syscall_handler.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/item_view_type.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

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
        db_proxy_),
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
    runtime_context_ = make_hogexec_context();
  }
}

void
SystemCallHandler::perform_initial_validation()
{
  // TODO(geoff): Adjust if some assumption on the rest of the system changes
  //
  // At this point, we do not know the provenance of signed_tx_.  As such,
  // an attached wasm may or may not have been validated by this point.
  //
  // We should validate wasms as they are sent to us from clients (before
  // broadcasting to other nodes) and we may wish to validate them as they are
  // broadcast to us from other nodes. But until we have a concrete design of
  // the coordinator and know what is is and is not doing, we conservatively
  // validate wasms here.
  //
  // If we make the assumption that all wasms are validated by the time they get
  // to the executor, this check could be removed.

  for (auto const &contract : signed_tx_.tx.contracts_to_deploy) {
    auto res = runtime_context_->verify_contract(contract);
    // TODO(geoff): gas costs for verifying contracts
    if (!res) {
      cumulative_invoke_result_.result_ =
          InvokeError{InvokeErrorCode::kInvokeValidationFailure};
    }
  }
}

void
SystemCallHandler::check_final_validation_data()
{
  assert(cumulative_invoke_result_.is_root_success());
  if (std::holds_alternative<ValidationModeData>(execution_mode_data_)) {
    auto &validation_data = std::get<ValidationModeData>(execution_mode_data_);
    if (!validation_data.validate()) {
      cumulative_invoke_result_.result_ =
          InvokeError{InvokeErrorCode::kInvokeValidationFailure};
    }
  }
}

InvokeResult
SystemCallHandler::root_invoke()
{
  perform_initial_validation();
  // if validation fails, cumulative_invoke_result_ is set to the appropriate
  // error and we return it immediately
  if (!cumulative_invoke_result_.is_root_success()) {
    return cumulative_invoke_result_;
  }

  auto runtime = resolve_runtime(signed_tx_.tx.invocation.invoked_address);
  if (!runtime) {
    // some system error when resolving the runtime
    cumulative_invoke_result_.result_ = runtime.error().to_invoke_error();
    return cumulative_invoke_result_;
  }
  if (!runtime.value()) {
    cumulative_invoke_result_.result_ =
        InvokeError{InvokeErrorCode::kInvokeDeterministic};
    return cumulative_invoke_result_;
  }

  GasAmount available_gas = cumulative_invoke_result_.gas_consumed_ <
                                    signed_tx_.tx.invocation.gas_limit
                                ? signed_tx_.tx.invocation.gas_limit -
                                      cumulative_invoke_result_.gas_consumed_
                                : 0;

  invocation_frame_stack_.start_frame(
      signed_tx_.tx.invocation.invoked_address,
      Calldata(signed_tx_.tx.invocation.calldata),
      std::to_underlying(InvokeFlag::kInvokeFlagNone));

  auto res = runtime.value()->invoke(sdk_method_id(
                                         signed_tx_.tx.invocation.entrypoint),
                                     available_gas);
  cumulative_invoke_result_.gas_consumed_ += res.gas_consumed_;

  // finish_status takes res as input and shortcircuits if res is an error
  auto finish_status =
      invocation_frame_stack_.finish_frame(res, execution_mode_data_);
  if (!finish_status.has_value()) {
    cumulative_invoke_result_.result_ = finish_status.error().to_invoke_error();
    return cumulative_invoke_result_;
  }
  res = finish_status.value();

  // should not have gotten here without being in a success state to begin with
  assert(cumulative_invoke_result_.is_root_success());
  cumulative_invoke_result_.result_ = res.result_;
  if (!cumulative_invoke_result_.is_root_success()) {
    return cumulative_invoke_result_;
  }

  // update cumulative_invoke_result_ with the final validation data
  check_final_validation_data();

  return cumulative_invoke_result_;
}

SystemStatus<bool>
SystemCallHandler::try_commit_transaction()
{
  assert(!transaction_committed_or_aborted_);
  if (!cumulative_invoke_result_.is_root_success()) {

    auto st = db_proxy_.abort_transaction();
    if (!st.has_value()) {
      return st.error();
    }

    return false;
  }
  assert(last_frame_.has_value());

  auto r = last_frame_.value().try_acquire_frame_locks(db_proxy_);
  if (!r.has_value()) {
    return r.error();
  }

  bool result = r.value();

  if (!result) {
    auto res = db_proxy_.abort_transaction();
    if (!res.has_value()) {
      return res.error();
    }
    transaction_committed_or_aborted_ = true;
    return false;
  }

  // check if any of the 2pc database operations failed
  auto res = db_proxy_.commit_transaction();
  if (!res.has_value()) {
    return res.error();
  }

  std::unordered_set<SHA256Hash> committed_contracts;
  for (auto const &[addr, hash] :
       last_frame_.value().new_contract_deployments_) {
    auto contract_entry = newly_created_contracts_.find(hash);
    assert(contract_entry != newly_created_contracts_.end());

    auto res = db_proxy_.commit_contract_deploy(addr, hash);
    if (!res.has_value()) {
      return res.error();
    }
    if (committed_contracts.insert(hash).second) {
      // contract that wasn't already created within the transaction
      res = db_proxy_.commit_contract_create(*contract_entry->second);
      if (!res.has_value()) {
        return res.error();
      }
    }
  }
  transaction_committed_or_aborted_ = true;
  return true;
}

// can only be called when call_frame_stack_ is nonempty
// (i.e. while at least one invoke() is running)
ContractAddress
SystemCallHandler::get_current_address() const
{
  return invocation_frame_stack_.get_active_frame().address();
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

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
SystemCallHandler::invoke(ContractAddress const &address, MethodID method,
                          GasAmount gas_limit, Calldata calldata,
                          InvokeFlagSet flags)
{
  auto runtime = resolve_runtime(address);
  if (!runtime) {
    return std::pair{runtime.error().to_host_fn_error(), 0};
  }
  if (!runtime.value()) {
    return std::pair{SyscallErrorCode::kContractLookupFailed, 0};
  }

  invocation_frame_stack_.start_frame(address, std::move(calldata), flags);

  auto res = runtime.value()->invoke(method, gas_limit);
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
  return runtime_context_->make_new_runtime(*load_contract, *deployed_hash,
                                            this, false);
}

Calldata const &
SystemCallHandler::get_calldata() const
{
  return invocation_frame_stack_.get_active_frame().calldata();
}

ProposalModeData const &
SystemCallHandler::get_proposal_data() const
{
  assert(std::holds_alternative<ProposalModeData>(execution_mode_data_));
  return std::get<ProposalModeData>(execution_mode_data_);
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
SystemCallHandler::get_bytestring(StorageKeyContractData const &storage_key)
{
  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  if (auto const *bytestring_item = item_view->get<BytestringItem>()) {
    return std::as_bytes(std::span(bytestring_item->bytestring_));
  }

  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (!unmodified_item->is_loaded()) {
      auto addrkey = invocation_frame_stack_.get_addrkey(storage_key);
      auto load_status = unmodified_item->load(snapshot_cache_, addrkey);
      if (!load_status.has_value()) {
        return load_status.error();
      }
    }
    auto const &base_item = unmodified_item->base_item();
    auto type = base_item.type();
    if (!type) {
      return SyscallError{SyscallErrorCode::kStorageKeyNotExist};
    }
    if (*type != ItemViewType::kBytestring) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    return std::as_bytes(std::span(base_item.bytestring()));
  }

  return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_bytestring(StorageKeyContractData const &storage_key,
                                  std::span<const std::byte> input_bytestring)
{
  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  Bytestring new_string;
  new_string.resize_uninitialized(input_bytestring.size());
  std::memcpy(new_string.data(),
              reinterpret_cast<const uint8_t *>(input_bytestring.data()),
              input_bytestring.size());

  return item_view->set(BytestringItem(std::move(new_string)));
}

SystemStatus<SyscallStatus<std::pair<int128_t, int128_t>>>
SystemCallHandler::get_nnint(StorageKeyContractData const &storage_key)
{
  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  if (auto const *nnint_item = item_view->get<NNIntItem>()) {
    if (nnint_item->base_value_.has_value()) {
      return {std::make_pair(*nnint_item->base_value_, nnint_item->add_value_)};
    }
    auto addrkey = invocation_frame_stack_.get_addrkey(storage_key);
    auto snapshot_value = snapshot_cache_.try_get_snapshot_item(addrkey);
    if (!snapshot_value.has_value()) {
      return snapshot_value.error();
    }
    // defaulting to 0, instead of returning an error, because there's already
    // an NNintItem at this storage location, even if the snapshot value is
    // different type.
    auto snapshot_type = snapshot_value.value()->type();
    if (!snapshot_type || snapshot_type.value() != ItemViewType::kNNInt) {
      return std::make_pair(0, nnint_item->add_value_);
    }
    return {std::make_pair(snapshot_value.value()->nnint(),
                           nnint_item->add_value_)};
  }

  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (!unmodified_item->is_loaded()) {
      auto addrkey = invocation_frame_stack_.get_addrkey(storage_key);
      auto load_status = unmodified_item->load(snapshot_cache_, addrkey);
      if (!load_status.has_value()) {
        return load_status.error();
      }
    }
    auto const &base_item = unmodified_item->base_item();
    auto type = base_item.type();
    if (!type) {
      return SyscallError{SyscallErrorCode::kStorageKeyNotExist};
    }
    if (*type != ItemViewType::kNNInt) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    return {std::make_pair(base_item.nnint(), 0)};
  }

  return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_nnint(StorageKeyContractData const &storage_key,
                             int128_t set_value, int128_t add_value)
{
  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  return item_view->set(
      NNIntItem{.base_value_ = set_value, .add_value_ = add_value});
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::add_nnint(StorageKeyContractData const &storage_key,
                             int128_t add_value)
{
  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  return item_view->set(
      NNIntItem{.base_value_ = std::nullopt, .add_value_ = add_value});
}
SystemStatus<SyscallStatus<void>>
SystemCallHandler::delete_object(StorageKeyContractData const &storage_key)
{
  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  return item_view->set(DeletedItem{});
}

namespace {
auto
queue_entry_from_span(std::span<const std::byte> span)
{
  QueueEntry new_entry;
  new_entry.resize_uninitialized(span.size());
  std::memcpy(new_entry.data(), reinterpret_cast<const uint8_t *>(span.data()),
              span.size());
  return new_entry;
}

SystemStatus<SyscallStatus<RetrocausalQueueItem *>>
retrocausal_queue_item_from_view(ItemView *item_view,
                                 AddressAndKey const &addrkey,
                                 StorageSnapshotCache &snapshot_cache)
{
  if (auto *queue_item = item_view->get<RetrocausalQueueItem>()) {
    return queue_item;
  }
  if (auto const *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (!unmodified_item->is_loaded()) {
      auto load_status = unmodified_item->load(snapshot_cache, addrkey);
      if (!load_status.has_value()) {
        return load_status.error();
      }
    }
    RetrocausalQueueItem new_item(unmodified_item->base_item());

    auto set_status = item_view->set(new_item);
    if (!set_status.has_value()) {
      return set_status.error();
    }
    return item_view->get<RetrocausalQueueItem>();
  }
  return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
}
} // namespace

SystemStatus<SyscallStatus<void>>
SystemCallHandler::retrocausal_queue_insert(
    StorageKeyContractData const &key, std::span<const std::byte> queue_entry)
{
  auto addrkey = invocation_frame_stack_.get_addrkey(key);
  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  auto queue_item =
      retrocausal_queue_item_from_view(item_view, addrkey, snapshot_cache_);
  if (!queue_item.has_value()) {
    return queue_item.error();
  }
  if (!queue_item.value().has_value()) {
    return queue_item.value().error();
  }

  // Now we have either grabbed the preexisting RCQItem, or created a new one.
  // In either case, we can now insert the new entry.
  queue_item.value().value()->insert_entry(queue_entry_from_span(queue_entry));
  return {};
}

SystemStatus<SyscallStatus<QueueEntry>>
SystemCallHandler::retrocausal_queue_consume(StorageKeyContractData const &key,
                                             bool peek)
{
  auto addrkey = invocation_frame_stack_.get_addrkey(key);
  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  auto queue_item =
      retrocausal_queue_item_from_view(item_view, addrkey, snapshot_cache_);
  if (!queue_item.has_value()) {
    return queue_item.error();
  }
  if (!queue_item.value().has_value()) {
    return queue_item.value().error();
  }

  auto entry =
      queue_item.value().value()->get_next_item(execution_mode_data_, !peek);
  if (!entry.has_value()) {
    return entry.error();
  }

  auto &value = entry.value();
  if (value.has_value()) {
    return value.value();
  }
  return SyscallError{SyscallErrorCode::kNoRetrocausalQueueEntry};
}

SystemStatus<SyscallStatus<int128_t>>
SystemCallHandler::get_native_token_balance(
    ContractAddress const &contract_address)
{
  AddressAndKey addrkey{
      .address = contract_address,
      .key = StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};

  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto *unmodified_item = item_view->get<UnmodifiedItem>()) {
    if (!unmodified_item->is_loaded()) {
      auto load_status = unmodified_item->load(snapshot_cache_, addrkey);
      if (!load_status.has_value()) {
        return load_status.error();
      }
    }
    auto const &base_item = unmodified_item->base_item();
    assert(base_item.type() == ItemViewType::kNativeTokenBalance);
    assert(base_item.native_token_balance() >= 0);
    return base_item.native_token_balance();
  }

  auto const *balance_item = item_view->get<NativeTokenBalanceItem>();
  assert(balance_item);

  auto base = [&] -> SystemStatus<int128_t> {
    if (balance_item->base_value_.has_value()) {
      return *balance_item->base_value_;
    }
    auto snapshot_value = snapshot_cache_.try_get_snapshot_item(addrkey);
    if (!snapshot_value.has_value()) {
      return snapshot_value.error();
    }
    auto snapshot_type = snapshot_value.value()->type();
    if (!snapshot_type.has_value()) {
      return 0;
    }
    assert(snapshot_type.value() == ItemViewType::kNativeTokenBalance);
    return snapshot_value.value()->native_token_balance();
  }();

  if (!base.has_value()) {
    return base.error();
  }
  if (integer_add_overflow(*base, balance_item->add_value_)) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  int128_t balance = *base + balance_item->add_value_;
  assert(balance >= 0);
  return balance;
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::modify_native_token_balance(
    ContractAddress const &contract_address, int128_t transfer_amount)
{
  AddressAndKey addrkey{
      .address = contract_address,
      .key = StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};

  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto const *balance_item = item_view->get<NativeTokenBalanceItem>()) {
    auto current_add = balance_item->add_value_;
    if (integer_add_overflow(current_add, transfer_amount)) {
      return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
    }
    auto new_add = current_add + transfer_amount;
    return item_view->set(NativeTokenBalanceItem(
        NNIntItem{.base_value_ = balance_item->base_value_,
                  .add_value_ = new_add}));
  }

  assert(item_view->get<UnmodifiedItem>());
  return item_view->set(NativeTokenBalanceItem(
      NNIntItem{.base_value_ = std::nullopt, .add_value_ = transfer_amount}));
}

} // namespace hogd
