#include "hogd/hogexec/runtime/syscall_handler.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <variant>

#include "account.hh"
#include "contract.hh"
#include "event.hh"
#include "executor_to_db_rpc.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/event.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/item_view_type.h"
#include "hogd/hogexec/runtime/metrics.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "retrocausal_queue.hh"
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/format.h"

namespace hogd {

namespace {
template<StorageItemRPCFlags... Flags>
constexpr bool
validate_flags(StorageItemRPCFlagSet flags)
{
  constexpr StorageItemRPCFlagSet kAllowedFlags =
      (0 | ... | std::to_underlying(Flags));
  return (flags & kAllowedFlags) == flags;
}
} // namespace

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
  assert(runtime_context_);
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

  GasAmount available_gas = cumulative_invoke_result_.gas_consumed_ <
                                    signed_tx_.tx.invocation.gas_limit
                                ? signed_tx_.tx.invocation.gas_limit -
                                      cumulative_invoke_result_.gas_consumed_
                                : 0;

  auto [res, gas_consumed] =
      invoke(signed_tx_.tx.invocation.invoked_address,
             sdk_method_id(signed_tx_.tx.invocation.entrypoint), available_gas,
             Calldata(signed_tx_.tx.invocation.calldata),
             std::to_underlying(InvokeFlag::kInvokeFlagNone));

  auto handle_root_host_fn_status =
      [](HostFnStatus<SyscallErrorCode> status) -> InvokeStatus<void> {
    if (status.has_value()) {
      // TODO(geoff): Consider whether we want to have "frame reverted" on the
      // root frame as a "transaction failed" or "transaction succeeded"
      return (status.value() == SyscallErrorCode::kSyscallErrorCodeNone ||
              status.value() ==
                  SyscallErrorCode::kSyscallErrorCodeFrameReverted)
                 ? InvokeStatus<void>{}
                 : InvokeError{InvokeErrorCode::kInvokeDeterministic};
    }
    switch (status.error().code()) {
    case HostFnErrorCode::kHostOutOfGas:
      return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
    case HostFnErrorCode::kHostReturn:
      return InvokeStatus<void>{};
    case HostFnErrorCode::kHostExit:
      return InvokeError{InvokeErrorCode::kInvokeDeterministic};
    case HostFnErrorCode::kHostValidationFailure:
      return InvokeError{InvokeErrorCode::kInvokeValidationFailure};
    case HostFnErrorCode::kHostAbortTransaction:
      return InvokeError{InvokeErrorCode::kInvokeAbortTransaction};
    case HostFnErrorCode::kHostUnrecoverable:
      return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
    case HostFnErrorCode::kHostNoneOrRecoverable:
      // This case should never happen, as it is instead handled by
      // the status.has_value() check above.
      assert(false);
      return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
    }
    // Leaving the default arm out of the switch, so that the compiler
    // will raise a warning later if we add additional InvokeError codes.
    return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
  };

  cumulative_invoke_result_.result_ = handle_root_host_fn_status(res);
  cumulative_invoke_result_.gas_consumed_ += gas_consumed;

  if (!cumulative_invoke_result_.is_root_success()) {
    return cumulative_invoke_result_;
  }

  // update cumulative_invoke_result_ with the final validation data
  check_final_validation_data();

  if (cumulative_invoke_result_.is_root_success()) {
    root_invoke_succeeded_ = true;
  }

  return cumulative_invoke_result_;
}

SystemStatus<bool>
SystemCallHandler::try_commit_transaction()
{
  assert(!transaction_committed_or_aborted_);
  if (!cumulative_invoke_result_.is_root_success()) {
    HogexecMetrics::mark_tx_failed(cumulative_invoke_result_,
                                   get_block_execution_mode());
    auto st = db_proxy_.abort_transaction();
    if (!st.has_value()) {
      return st.error();
    }

    return false;
  }
  assert(last_frame_.has_value());
  assert(root_invoke_succeeded_);

  // If the last frame was set to manually revert, then we commit the
  // transaction as a no-op.
  if (last_frame_ // NOLINT(bugprone-unchecked-optional-access)
          .value()
          .should_revert()) {
    auto res = db_proxy_.commit_transaction({});
    HogexecMetrics::mark_tx_failed(cumulative_invoke_result_,
                                   get_block_execution_mode());
    if (!res.has_value()) {
      return res.error();
    }

    transaction_committed_or_aborted_ = true;
    return true;
  }

  auto r = last_frame_ // NOLINT(bugprone-unchecked-optional-access)
               .value()
               .try_acquire_frame_locks(db_proxy_);
  if (!r.has_value()) {
    return r.error();
  }

  bool result = r.value();
  // check if any of the 2pc database operations failed
  if (!result) {
    auto res = db_proxy_.abort_transaction();
    HogexecMetrics::mark_tx_failed(cumulative_invoke_result_,
                                   get_block_execution_mode());

    if (!res.has_value()) {
      return res.error();
    }

    transaction_committed_or_aborted_ = true;
    return false;
  }

  auto new_contract_hashes =
      last_frame_ // NOLINT(bugprone-unchecked-optional-access)
          .value()
          .get_committed_contracts();
  NewContractBatch new_contracts;
  for (auto const &hash : new_contract_hashes) {
    auto contract_entry = newly_created_contracts_.find(hash);
    if (contract_entry != newly_created_contracts_.end()) {
      new_contracts.push_back(*contract_entry->second);
    }
  }

  auto res = db_proxy_.commit_transaction(std::move(new_contracts));
  if (!res.has_value()) {
    return res.error();
  }

  HogexecMetrics::mark_tx_success(cumulative_invoke_result_,
                                  get_block_execution_mode());
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
  return res.has_value()
             ? SyscallStatus<ContractAddress>(*res)
             : SyscallError{
                   SyscallErrorCode::kSyscallErrorCodeIsRootTransaction};
}

std::pair<HostFnStatus<SyscallErrorCode>, GasAmount>
SystemCallHandler::invoke(ContractAddress const &address, MethodID method,
                          GasAmount gas_limit, Calldata calldata,
                          InvokeFlagSet flags)
{
  auto runtime = resolve_runtime(address);
  if (!runtime) {
    trace("SystemError: resolving runtime for address: {}, error {}",
          x23::show(address), runtime.error());
    return std::pair{runtime.error().to_host_fn_error(), 0};
  }
  if (!runtime.value()) {
    trace("Failed to instantiate runtime for address: {}", x23::show(address));
    return std::pair{SyscallErrorCode::kSyscallErrorCodeContractLookupFailed,
                     0};
  }

  if (!invocation_frame_stack_.empty() && is_read_only()) {
    flags |= std::to_underlying(InvokeFlag::kInvokeFlagReadOnly);
  }

  InvokeResult cumulative_result = {
      .result_ = InvokeStatus<void>{},
      .gas_consumed_ = 0,
  };

  invocation_frame_stack_.start_frame(address, std::move(calldata), flags);

  cumulative_result += runtime.value()->invoke(method, gas_limit);
  auto finish_status =
      invocation_frame_stack_.finish_frame(cumulative_result.result_,
                                           execution_mode_data_);
  if (!finish_status.has_value()) {
    // TODO(geoff): This status will result ending execution of the whole block.
    // We can add gas consumption here, but that would only affect the state of
    // the local scheduler, not the canonical chain.
    return {finish_status.error().to_host_fn_error(), 0};
  }
  auto [frame_result, manually_reverted] = finish_status.value();
  cumulative_result += frame_result;

  auto out = cumulative_result.to_host_fn_result();
  if (out.first.has_value() &&
      out.first.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
    if (manually_reverted) {
      out.first = SyscallErrorCode::kSyscallErrorCodeFrameReverted;
    }
  }

  return out;
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
             : SyscallError{
                   SyscallErrorCode::kSyscallErrorCodeNoReturnBufferAvailable};
}

void
SystemCallHandler::set_return_buffer(ReturnBuffer &&returned_buffer,
                                     bool revert_frame)
{
  assert(!invocation_frame_stack_.empty());
  invocation_frame_stack_.get_active_frame().set_frame_return(
      std::move(returned_buffer));

  if (revert_frame) {
    invocation_frame_stack_.get_active_frame().set_should_revert();
  }
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

NondeterministicProposalData const &
SystemCallHandler::get_proposal_data() const
{
  assert(root_invoke_succeeded_);
  return std::visit(
      [](auto const& arg) -> NondeterministicProposalData const& { return arg.proposal_data(); }, execution_mode_data_);
}


SHA256Hash
SystemCallHandler::get_proposed_tx_hash() const
{
  assert(root_invoke_succeeded_);

  ProposedTransaction proposed_tx {
    .signed_tx = signed_tx_,
    .nondeterministic_proposal_data = get_proposal_data()
  };

  return sha256(proposed_tx);
}

void
SystemCallHandler::log_event(SHA256Hash const *topic0, SHA256Hash const *topic1,
                             SHA256Hash const *topic2,
                             std::span<const std::byte> data)
{
  assert(!invocation_frame_stack_.empty());
  assert(!is_read_only());
  assert(data.size() <= kMaxLogMessageLen);

  auto message = make_tx_emitted_event(
      invocation_frame_stack_.get_active_frame().address(), topic0, topic1,
      topic2, data);

  invocation_frame_stack_.get_active_frame().log_event(std::move(message));
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
  assert(!is_read_only());

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
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeContractLookupFailed};
    }
    load_contract = it->second;
  }
  assert(load_contract != nullptr);

  auto deploy_addr =
      static_get_deploy_address(get_current_address(), nonce, contract_hash);

  auto deploy_check_status =
      snapshot_cache_.try_get_deployed_contract_hash(deploy_addr);
  if (!deploy_check_status) {
    return deploy_check_status.error();
  }
  if (*deploy_check_status) {
    return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeContractAlreadyDeployed};
  }

  if (invocation_frame_stack_.lookup_locally_deployed_contract(deploy_addr)
          .has_value()) {
    // contract has already been deployed in this transaction
    return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeContractAlreadyDeployed};
  }

  auto &frame = invocation_frame_stack_.get_active_frame();

  auto contract_size = [&] -> uint32_t {
    switch (load_contract->contract_type()) {
    case ContractType::kNative:
    case ContractType::kMockTest:
      return 0LU;
    case ContractType::kWasmi:
      return load_contract->wasmi_contract().wasm.size();
    default:
      assert(false);
      return 0LU;
    }
  }();

  // deploy new contract
  frame.deploy_contract(deploy_addr,
                        DeployedContractHash{.hash = contract_hash,
                                             .contract_size = contract_size});

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
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
    }
    if (*type != ItemViewType::kBytestring) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    return std::as_bytes(std::span(base_item.bytestring()));
  }

  return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
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

SystemStatus<SyscallStatus<std::pair<std::optional<int128_t>, int128_t>>>
SystemCallHandler::get_nnint(StorageKeyContractData const &storage_key,
                             bool load_from_snapshot)
{
  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  if (auto const *nnint_item = item_view->get<NNIntItem>()) {
    if (nnint_item->base_value_.has_value()) {
      return {std::make_pair(*nnint_item->base_value_, nnint_item->add_value_)};
    }
    // If we're not loading from the snapshot, we can return nullopt for the
    // base value.
    if (!load_from_snapshot) {
      return {std::make_pair(std::nullopt, nnint_item->add_value_)};
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
    if (!load_from_snapshot) {
      return {std::make_pair(std::nullopt, 0)};
    }

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
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
    }
    if (*type != ItemViewType::kNNInt) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    return {std::make_pair(base_item.nnint(), 0)};
  }

  return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_nnint(StorageKeyContractData const &storage_key,
                             std::optional<int128_t> set_value,
                             int128_t add_value)
{
  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  // Writing to an nnint, without explicitly defining the set value,
  // is defined as equivalent to outputting set_add with set equal to
  // whatever the current database snapshot value is.  As such, it is correct to
  // overwrite the set value here with nullopt, in such a case.
  return item_view->set(
      NNIntItem{.base_value_ = set_value, .add_value_ = add_value});
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::delete_object(StorageKeyContractData const &storage_key,
                                 StorageItemRPCFlagSet flags)
{
  assert(!is_read_only());

  if (!validate_flags<StorageItemRPCFlags::kStorageExclusiveLock>(flags)) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);
  return item_view->set(DeletedItem{}, flags);
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
  return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
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
  return SyscallError{
      SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry};
}

// == Native token syscalls ==

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
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  int128_t balance = *base + balance_item->add_value_;
  assert(balance >= 0);
  return balance;
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::modify_native_token_balance(
    ContractAddress const &contract_address, int128_t transfer_amount)
{
  assert(!invocation_frame_stack_.empty());
  assert(!is_read_only());

  AddressAndKey addrkey{
      .address = contract_address,
      .key = StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};

  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto const *balance_item = item_view->get<NativeTokenBalanceItem>()) {
    auto current_add = balance_item->add_value_;
    if (integer_add_overflow(current_add, transfer_amount)) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
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

// == Authentication syscalls ==

SystemStatus<SyscallStatus<void>>
SystemCallHandler::change_public_key_registration(uint32_t key_index,
                                                  PublicKeyConfigOpt new_key)
{
  assert(!invocation_frame_stack_.empty());
  auto addr = invocation_frame_stack_.get_active_frame().address();
  AddressAndKey addrkey{.address = addr,
                        .key = StorageKey(
                            StorageKeyType::kStorageKeyTypeAccountAuthData)};

  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto *account_auth_data_item = item_view->get<AccountAuthDataItem>()) {
    account_auth_data_item->change_public_key(key_index, std::move(new_key));
    return {};
  }

  assert(item_view->get<UnmodifiedItem>());
  AccountAuthDataItem new_item;
  new_item.change_public_key(key_index, std::move(new_key));
  return item_view->set(new_item);
}

} // namespace hogd
