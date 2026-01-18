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

#include "address_auth.hh"
#include "auth_claim.hh"
#include "contract.hh"
#include "event.hh"
#include "exec_result.hh"
#include "executor_to_db_rpc.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/utilities.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/item_view_type.h"
#include "hogd/hogexec/runtime/metrics.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "precompiles.hh"
#include "retrocausal_queue.hh"
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/format.h"
#include "x23/vec.h"

namespace hogd {

ExecResult
SystemCallHandler::to_exec_result(InvokeErrorCode error_code)
{
  switch (error_code) {
  case InvokeErrorCode::kInvokeNone:
    return ExecResult::ExecResultOK;
  case InvokeErrorCode::kInvokeAbortTransaction:
    return ExecResult::ExecResultSystemError;
  case InvokeErrorCode::kInvokeDeterministic:
  case InvokeErrorCode::kInvokeOutOfGas:
    return ExecResult::ExecResultTxDeterministicError;
  case InvokeErrorCode::kInvokeValidationFailure:
    return ExecResult::ExecResultTxValidationFailure;
  case InvokeErrorCode::kInvokeUnrecoverable:
    error("make_exec_result: received kInvokeUnrecoverable");
    return ExecResult::ExecResultUnrecoverable;
  }
  assert(false);
  return ExecResult::ExecResultUnrecoverable;
}

ExecResult
SystemCallHandler::to_exec_result(SystemErrorCode error_code)
{
  switch (error_code) {
  case SystemErrorCode::kSystemUnrecoverable:
    return ExecResult::ExecResultUnrecoverable;
  case SystemErrorCode::kSystemAbortTransaction:
    return ExecResult::ExecResultSystemError;
  case SystemErrorCode::kSystemValidationFailure:
    return ExecResult::ExecResultTxValidationFailure;
  }
  assert(false);
  return ExecResult::ExecResultUnrecoverable;
}

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
    PerTransactionDatabaseProxy db_proxy,
    PerTransactionEventServerProxy event_server_proxy,
    ExecutionModeCtor execution_mode_ctor, RuntimeContext &runtime_context)
  : runtime_context_(runtime_context), signed_tx_(std::move(signed_tx)),
    db_proxy_(std::move(db_proxy)),
    event_server_proxy_(std::move(event_server_proxy)),
    invocation_frame_stack_(
        [this](InvocationFrame &&last_frame) {
          commit_last_call_frame_success(std::move(last_frame));
        },
        db_proxy_),
    newly_created_contracts_([&] {
      std::unordered_map<SHA256Hash, std::unique_ptr<const Contract>> contracts;

      for (auto const &contract : signed_tx_.tx.contracts_to_deploy) {
        contracts.emplace(sha256(contract),
                          std::make_unique<const Contract>(contract));
      }
      return contracts;
    }()),
    execution_mode_data_(
        [](ExecutionModeCtor execution_mode_ctor) -> ExecutionModeData {
          if (std::holds_alternative<ProposalModeCtor>(execution_mode_ctor)) {
            return ProposalModeData{};
          }
          return ExecutionModeData{std::in_place_type<ValidationModeData>,
                                   std::get<ValidationModeCtor>(
                                       execution_mode_ctor)};
        }(std::move(execution_mode_ctor))),
    snapshot_cache_(snapshot_cache)
{}

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
    auto res = runtime_context_.verify_contract(contract);
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
      error("handle_root_host_fn_status: received kHostUnrecoverable");
      return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
    case HostFnErrorCode::kHostNoneOrRecoverable:
      // This case should never happen, as it is instead handled by
      // the status.has_value() check above.
      assert(false);
      return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
    }
    // Leaving the default arm out of the switch, so that the compiler
    // will raise a warning later if we add additional InvokeError codes.
    error("handle_root_host_fn_status: received unknown error code {}",
          status.error().code());
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

ExecResult
SystemCallHandler::try_commit_transaction()
{
  assert(!transaction_committed_or_aborted_);
  if (!cumulative_invoke_result_.is_root_success()) {
    if (cumulative_invoke_result_.result_.error().code() ==
        InvokeErrorCode::kInvokeUnrecoverable) {
      error("try_commit_transaction: received kInvokeUnrecoverable");
    }

    auto res = abort_transaction();
    if (!res.has_value()) {
      return to_exec_result(res.error().code());
    }

    return to_exec_result(cumulative_invoke_result_.result_.error().code());
  }
  assert(last_frame_.has_value());
  assert(root_invoke_succeeded_);
  assert(cumulative_invoke_result_.result_.has_value());

  // If the last frame was set to manually revert, then we commit the
  // transaction as a no-op.
  if (last_frame_ // NOLINT(bugprone-unchecked-optional-access)
          .value()
          .should_revert()) {
    auto res = db_proxy_.commit_transaction({});
    HogexecMetrics::mark_tx_failed(cumulative_invoke_result_,
                                   get_block_execution_mode());
    if (!res.has_value()) {
      return to_exec_result(res.error().code());
    }

    // extract the events, so that they cannot be sent later to the event server
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    last_frame_.value().extract_events();

    transaction_committed_or_aborted_ = true;
    return ExecResult::ExecResultTxRootFrameReverted;
  }

  auto r = last_frame_ // NOLINT(bugprone-unchecked-optional-access)
               .value()
               .try_acquire_frame_locks(db_proxy_);
  if (!r.has_value()) {
    return to_exec_result(r.error().code());
  }

  bool result = r.value();
  // check if any of the 2pc database operations failed
  if (!result) {
    two_phase_commit_failed_ = true;

    auto abort_status = abort_transaction();
    if (!abort_status.has_value()) {
      return to_exec_result(abort_status.error().code());
    }

    // abort_transaction() sets transaction_committed_or_aborted_ to true
    return ExecResult::ExecResultTxNonDeterministicError;
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
    return to_exec_result(res.error().code());
  }

  // This is past the point of successful commitment;
  // now we can safely emit the events to the event server

  HogexecMetrics::mark_tx_success(cumulative_invoke_result_,
                                  get_block_execution_mode());
  transaction_committed_or_aborted_ = true;
  return ExecResult::ExecResultOK;
}

SystemStatus<void>
SystemCallHandler::abort_transaction()
{
  assert(!transaction_committed_or_aborted_);
  transaction_committed_or_aborted_ = true;
  HogexecMetrics::mark_tx_failed(cumulative_invoke_result_,
                                 get_block_execution_mode());
  return db_proxy_.abort_transaction();
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
SystemCallHandler::invoke(ContractAddress const &address,
                          Runtime::MethodID method, GasAmount gas_limit,
                          Calldata calldata, InvokeFlagSet flags)
{
  auto runtime = resolve_runtime(address);
  if (!runtime) {
    trace("SystemError: resolving runtime for address: {}, error {}",
          x23::show(address), runtime.error());
    return std::pair{runtime.error().to_host_fn_error(), 0};
  }
  if (!runtime.value()) {
    trace("Failed to instantiate runtime for address: {}", x23::show(address));
    return std::pair{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput, 0};
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

SystemStatus<const Contract *>
SystemCallHandler::resolve_contract(SHA256Hash const &hash) const
{
  auto snapshot_res = snapshot_cache_.try_get_contract_by_hash(hash);
  if (!snapshot_res.has_value()) {
    return snapshot_res.error();
  }
  auto &snapshot_contract = *snapshot_res;
  if (snapshot_contract != nullptr) {
    return snapshot_contract;
  }

  auto it = newly_created_contracts_.find(hash);
  if (it != newly_created_contracts_.end()) {
    assert(it->second != nullptr);
    return it->second.get();
  }
  return nullptr;
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

  // actually make the new runtime
  // TODO(geoff): do verification when contracts are created, not each time.
  return runtime_context_.make_new_runtime(*deployed_hash, this, false);
}

Calldata const &
SystemCallHandler::get_calldata() const
{
  return invocation_frame_stack_.get_active_frame().calldata();
}

NondeterministicProposalData const &
SystemCallHandler::get_proposal_data() const
{
  // Either succeeded on the root tx (with or without a revert)
  // or deterministic error during execution.
  // Not allowed to have i.e. validation failure here, or block abort.
  assert(cumulative_invoke_result_.has_readable_proposal_data());
  return std::visit([](auto const &arg) -> NondeterministicProposalData const
                                            & { return arg.proposal_data(); },
                    execution_mode_data_);
}

SHA256Hash
SystemCallHandler::get_proposed_tx_hash() const
{
  ProposedTransaction proposed_tx{.signed_tx = signed_tx_,
                                  .nondeterministic_proposal_data =
                                      get_proposal_data()};

  return sha256(proposed_tx);
}

x23::Async<SystemStatus<void>>
SystemCallHandler::emit_events()
{
  assert(last_frame_.has_value());
  assert(cumulative_invoke_result_.is_root_success());
  assert(root_invoke_succeeded_);
  auto events = last_frame_.value().extract_events();
  if (events.empty()) {
    trace("No events to emit, skipping emit_events RPC");
    co_return SystemStatus<void>{};
  }
  co_return co_await event_server_proxy_.emit_events(TxEventLog{
      .proposed_tx_hash = get_proposed_tx_hash(),
      .events = std::move(events),
  });
}

SyscallStatus<void>
SystemCallHandler::log_event(Topic const *topic0, Topic const *topic1,
                             Topic const *topic2,
                             std::span<const std::byte> data)
{
  assert(!invocation_frame_stack_.empty());
  assert(!is_read_only());

  x23::Buf buf;
  uint32_t num_topics = 0;
  if (topic0) {
    buf.append_range(*topic0);
    num_topics++;
  }
  if (topic1) {
    buf.append_range(*topic1);
    num_topics++;
  }
  if (topic2) {
    buf.append_range(*topic2);
    num_topics++;
  }

  buf.append(reinterpret_cast<const uint8_t *>(data.data()), data.size());

  if (buf.size() > kMaxEventLength) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto message = make_tx_emitted_event(
      invocation_frame_stack_.get_active_frame().address(), num_topics,
      std::as_bytes(std::span(buf)));

  invocation_frame_stack_.get_active_frame().log_event(std::move(message));
  return {};
}

SyscallStatus<void>
SystemCallHandler::log_event(uint32_t num_topics,
                             std::span<const std::byte> data)
{
  assert(!invocation_frame_stack_.empty());
  assert(!is_read_only());
  if (num_topics > kMaxTopics) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  if (data.size() > kMaxEventLength) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto message = make_tx_emitted_event(
      invocation_frame_stack_.get_active_frame().address(), num_topics, data);
  invocation_frame_stack_.get_active_frame().log_event(std::move(message));
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

namespace {
constexpr bool
is_banned_redeploy_native_contract(uint32_t native_contract_id)
{
  switch (static_cast<HogexecPrecompiledNativeContracts>(native_contract_id)) {
  case HogexecPrecompiledNativeContracts::kNativeContractHogToken:
    return true;
  }
  panic("Unknown native contract deployed: {}", native_contract_id);
}
} // namespace

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
  auto contract_res = resolve_contract(contract_hash);
  if (!contract_res.has_value()) {
    return contract_res.error();
  }
  if (contract_res.value() == nullptr) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  assert(contract_res.value() != nullptr);
  auto const &contract = *contract_res.value();

  // Native (precompiled) contracts cannot be deployed - they are set up at
  // genesis and should not be redeployed by user transactions
  if (contract.contract_type() == ContractType::kNative &&
      is_banned_redeploy_native_contract(contract.native_contract_id())) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

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
    switch (contract.contract_type()) {
    case ContractType::kNative:
    case ContractType::kMockTest:
      return 0LU;
    case ContractType::kWasmi:
      return contract.wasmi_contract().wasm.size();
    default:
      assert(false);
      return 0LU;
    }
  }();

  // deploy new contract
  frame.deploy_contract(deploy_addr,
                        DeployedContractHash{.hash = contract_hash,
                                             .contract_size = contract_size,
                                             .contract_type =
                                                 contract.contract_type()});

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

  if (item_view->get<DeletedItem>()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
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
                                  std::span<const std::byte> input_bytestring,
                                  StorageItemRPCFlagSet input_flags)
{
  assert(!is_read_only());

  if (!validate_flags<StorageItemRPCFlags::kStorageExclusiveLock>(
          input_flags)) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);
  assert(input_bytestring.size() <= kBytestringMaxLen);

  Bytestring new_string;
  new_string.resize_uninitialized(input_bytestring.size());
  if (!input_bytestring.empty()) {
    std::memcpy(new_string.data(),
                reinterpret_cast<const uint8_t *>(input_bytestring.data()),
                input_bytestring.size());
  }

  return item_view->set(BytestringItem(std::move(new_string)), input_flags);
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

    // In this case, we're returning the parameters of the prior NNInt write.
    // A bit of a weird case, because we're following an NNintSet(nullopt, add)
    // with a Get(force_load_from_snapshot=true).  We augment the prior NNint
    // write with what the set value will be, when the write is applied to the
    // database. Hence we default to 0 if the snapshot value is not an NNInt.
    auto snapshot_type = snapshot_value.value()->type();
    if (!snapshot_type || snapshot_type.value() != ItemViewType::kNNInt) {
      return std::make_pair(0, nnint_item->add_value_);
    }
    return {std::make_pair(snapshot_value.value()->nnint(),
                           nnint_item->add_value_)};
  }

  if (item_view->get<DeletedItem>()) {
    if (!load_from_snapshot) {
      return {std::make_pair(std::nullopt, 0)};
    }
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
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
      assert(load_from_snapshot);
      // We are in load_from_snapshot mode, so we return KeyNotExist instead of
      // std::nullopt, mirroring the behavior above of the DeletedItem case.
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeStorageKeyNotExist};
    }
    if (*type != ItemViewType::kNNInt) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    return {std::make_pair(base_item.nnint(), 0)};
  }

  // the value written at this key is not an NNInt, so we return an
  // InvalidSyscallInput error. This mirrors the case above, where the snapshot
  // *type != kNNInt.
  if (!load_from_snapshot) {
    return {std::make_pair(std::nullopt, 0)};
  }
  return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_nnint(StorageKeyContractData const &storage_key,
                             std::optional<int128_t> set_value,
                             int128_t add_value,
                             StorageItemRPCFlagSet input_flags)
{
  assert(!is_read_only());

  if (!validate_flags<StorageItemRPCFlags::kStorageExclusiveLock>(
          input_flags)) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto *item_view = invocation_frame_stack_.get_item_view(storage_key);
  assert(item_view);

  // Writing to an nnint, without explicitly defining the set value,
  // is defined as equivalent to outputting set_add with set equal to
  // whatever the current database snapshot value is.  As such, it is correct to
  // overwrite the set value here with nullopt, in such a case.
  return item_view->set(NNIntItem{.base_value_ = set_value,
                                  .add_value_ = add_value},
                        input_flags);
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
  if (!new_entry.empty()) {
    std::memcpy(new_entry.data(),
                reinterpret_cast<const uint8_t *>(span.data()), span.size());
  }
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

  // Possible that the RCQ was deleted and then re-created in this transaction.
  // In this case, we need to load the base item from the snapshot and create a
  // new RCQItem.
  if (item_view->get<DeletedItem>()) {
    // TODO(geoff): Fix, but this has to somehow account for the edge case of
    // deleting the queue and re-creating it. In that scenario, we shouldn't be
    // able to read queue entries from the snapshot queue. Correct behavior is
    // to treat that scenario as a queue that consumes the entirety of the prior
    // queue.
    panic("This behavior is incorrect");
    UnmodifiedItem unmodified_item;
    assert(!unmodified_item.is_loaded());
    auto load_status = unmodified_item.load(snapshot_cache, addrkey);
    if (!load_status.has_value()) {
      return load_status.error();
    }
    RetrocausalQueueItem new_item(unmodified_item.base_item());

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
  if (queue_entry.size() > kQueueEntryMaxLen) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
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

namespace {
AddressAndKey
auth_data_addrkey(ContractAddress const &address)
{
  return {.address = address,
          .key = StorageKey(StorageKeyType::kStorageKeyTypeAddressAuthData)};
}
} // namespace

SystemStatus<SyscallStatus<void>>
SystemCallHandler::change_public_key_registration(uint32_t key_index,
                                                  PublicKeyConfigOpt new_key)
{
  if (key_index >= kMaxSigners) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  assert(!invocation_frame_stack_.empty());
  auto addr = invocation_frame_stack_.get_active_frame().address();
  AddressAndKey addrkey = auth_data_addrkey(addr);

  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto *address_auth_data_item = item_view->get<AddressAuthDataItem>()) {
    address_auth_data_item->change_public_key(key_index, std::move(new_key));
    return {};
  }

  if (item_view->get<DeletedItem>()) [[unlikely]] {
    error("Unrecoverable: unexpected manual delete of authentication data");
    return SystemError{SystemErrorCode::kSystemUnrecoverable};
  }

  assert(item_view->get<UnmodifiedItem>());
  AddressAuthDataItem new_item;
  new_item.change_public_key(key_index, std::move(new_key));
  return item_view->set(new_item);
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_address_nonce_config(NonceConfig const &nonce_config)
{
  auto addr = invocation_frame_stack_.get_active_frame().address();
  auto addrkey = auth_data_addrkey(addr);
  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto *address_auth_data_item = item_view->get<AddressAuthDataItem>()) {
    address_auth_data_item->set_nonce_config(nonce_config);
    return {};
  }

  if (item_view->get<DeletedItem>()) [[unlikely]] {
    error("Unrecoverable: unexpected manual delete of authentication data");
    return SystemError{SystemErrorCode::kSystemUnrecoverable};
  }
  assert(item_view->get<UnmodifiedItem>());
  AddressAuthDataItem new_item;
  new_item.set_nonce_config(nonce_config);
  return item_view->set(new_item);
}

SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_address_gas_permission_configs(
    x23::Vec<GasPermissionConfig, kMaxGasConfigs> gas_permission_configs)
{
  auto addr = invocation_frame_stack_.get_active_frame().address();
  auto addrkey = auth_data_addrkey(addr);
  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto *address_auth_data_item = item_view->get<AddressAuthDataItem>()) {
    address_auth_data_item->set_gas_permission_configs(
        std::move(gas_permission_configs));
    return {};
  }

  if (item_view->get<DeletedItem>()) [[unlikely]] {
    error("Unrecoverable: unexpected manual delete of authentication data");
    return SystemError{SystemErrorCode::kSystemUnrecoverable};
  }

  assert(item_view->get<UnmodifiedItem>());
  AddressAuthDataItem new_item;
  new_item.set_gas_permission_configs(std::move(gas_permission_configs));
  return item_view->set(new_item);
}

SyscallStatus<void>
SystemCallHandler::sha256_update(uint32_t input_index,
                                 std::span<const std::byte> input_buffer,
                                 bool input_reset)
{
  assert(!invocation_frame_stack_.empty());
  return invocation_frame_stack_.get_active_frame().hash_buffers_.update(
      input_index, input_buffer, input_reset);
}

SyscallStatus<SHA256Hash>
SystemCallHandler::sha256_finalize(uint32_t input_index)
{
  assert(!invocation_frame_stack_.empty());
  size_t output_size = 0;
  auto r = invocation_frame_stack_.get_active_frame().hash_buffers_.finalize(
      input_index, &output_size);
  if (auto *stats = get_transaction_stats()) {
    stats->sha256_total_bytes_processed += output_size;
  }
  return r;
}

// == Block statistics syscalls ==

SystemStatus<BlockHeight>
SystemCallHandler::get_block_height() const
{
  auto tip = snapshot_cache_.try_get_db_tip();
  if (!tip.has_value()) {
    return tip.error();
  }
  return tip.value()->height;
}

SystemStatus<SHA256Hash>
SystemCallHandler::get_parent_block_hash() const
{
  auto tip = snapshot_cache_.try_get_db_tip();
  if (!tip.has_value()) {
    return tip.error();
  }
  return tip.value()->hash;
}

} // namespace hogd
