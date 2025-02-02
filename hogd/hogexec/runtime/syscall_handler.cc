#include "hogd/hogexec/runtime/syscall_handler.h"

#include <algorithm>
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
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/flag_utils.h"
#include "hogd/hogexec/runtime/invocation_frame.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

SystemCallHandler::SystemCallHandler(SignedTransaction signed_tx,
                                     StorageSnapshotCache &snapshot_cache,
                                     PerTransactionDatabaseProxy &db_proxy,
                                     ExecutionModeData execution_mode_data)
  : signed_tx_(std::move(signed_tx)),
    invocation_frame_stack_(
        [this](InvocationFrame &last_frame) {
          commit_last_call_frame_success(last_frame);
        },
        snapshot_cache, db_proxy),
    execution_mode_data_(std::move(execution_mode_data)),
    newly_created_contracts_([&] {
      std::unordered_map<SHA256Hash, std::shared_ptr<const Contract>> contracts;

      for (auto const &contract : signed_tx_.tx.contracts_to_deploy) {
        contracts.emplace(sha256(contract),
                          std::make_shared<const Contract>(contract));
      }
      return contracts;
    }()),
    snapshot_cache_(snapshot_cache), db_proxy_(db_proxy)
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
  res = invocation_frame_stack_.finish_frame(res, execution_mode_data_);
  res = finish_nondeterminism_checks(res);

  if (!res.is_root_success())
    return res.to_root_error();
  return res;
}

x23::Async<bool>
SystemCallHandler::try_commit_transaction()
{
  // TODO(geoff): do something with committed_logs_
  std::vector<x23::Async<bool>> reservations;
  reservations.reserve(committed_deltas_.size());
  for (auto const &[k, d] : committed_deltas_) {
    reservations.push_back(db_proxy_.try_reserve_delta(
        k, d, std::to_underlying(RPCFlags::kFinalCommit)));
  }

  for (auto &r : reservations) {
    if (!co_await r) {
      co_await db_proxy_.abort_transaction();
      co_return false;
    }
  }

  co_await db_proxy_.commit_transaction();

  std::vector<x23::Async<void>> commitments;
  commitments.reserve(newly_created_contracts_.size() +
                      committed_deployments_.size());
  for (auto const &[_, c] : newly_created_contracts_) {
    commitments.push_back(db_proxy_.create_contract(*c));
  }
  for (auto const &[a, h] : committed_deployments_) {
    commitments.push_back(db_proxy_.deploy_contract(a, h));
  }

  for (auto &v : commitments) {
    co_await v;
  }
  co_return true;
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
  res = invocation_frame_stack_.finish_frame(res, execution_mode_data_);

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
  committed_deltas_ = frame.extract_deltas();
  committed_deployments_ = frame.extract_deployments();
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

std::pair<std::optional<SHA256Hash>, bool>
SystemCallHandler::lookup_deployed_contract_hash(ContractAddress const &addr)
{
  // lookuup local first, just to save an rpc in some cases
  auto local_deployed_hash =
      invocation_frame_stack_.lookup_locally_deployed_contract(addr);
  if (local_deployed_hash) {
    return {local_deployed_hash, true};
  }
  return {snapshot_cache_.try_get_deployed_contract_hash(addr), false};
}

std::shared_ptr<const Contract>
SystemCallHandler::lookup_contract_by_hash(SHA256Hash const &hash)
{
  auto it = newly_created_contracts_.find(hash);
  if (it != newly_created_contracts_.end()) {
    return it->second;
  }
  return snapshot_cache_.try_get_contract_by_hash(hash);
}

RuntimeCacheEntry *
SystemCallHandler::resolve_runtime(ContractAddress const &addr)
{
  auto it = runtimes_.find(addr);
  if (it != runtimes_.end()) {
    auto *out = it->second.get();

    // The entry was created by the current transaction.
    // Must check whether that action was rolled back.
    if (out->local_deployment_ &&
        !invocation_frame_stack_.lookup_locally_deployed_contract(addr)) {
      // Runtime exists in the cache, but it was deployed in an earlier
      // call frame that has since been reverted.
      return nullptr;
    }

    return out->revert_poisoned_ ? nullptr : out;
  }

  // No runtime cache entry exists; now try to create a new one.
  auto [deployed_hash, local_deployment] = lookup_deployed_contract_hash(addr);
  if (!deployed_hash) {
    // A deployment was not found in the local context
    // nor in the snapshot.
    return nullptr;
  }

  // At this point, we have a deployment and the hash of the contract.
  // We now resolve the actual contract code from the snapshot.
  // Deployment makes sure that the contract exists.
  auto contract = lookup_contract_by_hash(*deployed_hash);
  assert(contract);

  // actually make the new runtime
  auto new_runtime =
      wasmi_context_.make_new_runtime(contract->contract_data, this);

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
    return SyscallError{SyscallErrorCode::kInvalidInput};
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
  auto load_contract = lookup_contract_by_hash(contract_hash);
  if (!load_contract) {
    return SyscallError{SyscallErrorCode::kContractLookupFailed};
  }

  auto deploy_addr =
      static_get_deploy_address(get_current_address(), nonce, contract_hash);

  auto [deployed_hash, _] = lookup_deployed_contract_hash(deploy_addr);
  if (deployed_hash) {
    return SyscallError{SyscallErrorCode::kContractAlreadyDeployed};
  }

  auto &frame = invocation_frame_stack_.get_active_frame();

  // deploy new contract
  frame.new_contract_deployments_.emplace(deploy_addr, contract_hash);

  return deploy_addr;
}

SyscallStatus<const Bytestring>
SystemCallHandler::get_bytestring(StorageKey const &storage_key)
{
  auto &item_view = invocation_frame_stack_.get_item_view(storage_key);

  if (auto const *bytestring_item = item_view.template get<BytestringItem>()) {
    return (bytestring_item->bytestring_);
  }

  if (auto const *unmodified_item = item_view.template get<UnmodifiedItem>()) {
    if (!unmodified_item->base_item_) {
      return SyscallError{SyscallErrorCode::kStorageKeyNotExist};
    }
    if (unmodified_item->base_item_->type() != StorageItemType::kBytestring) {
      return SyscallError{SyscallErrorCode::kInvalidInput};
    }
    return unmodified_item->base_item_->bytestring();
  }

  return SyscallError{SyscallErrorCode::kInvalidInput};
}

SyscallStatus<void>
SystemCallHandler::set_bytestring(StorageKey const &storage_key,
                                  std::span<const std::byte> input_bytestring,
                                  StorageDeltaFlags flags)
{
  auto &item_view = invocation_frame_stack_.get_item_view(storage_key);

  Bytestring new_string;
  new_string.resize_uninitialized(input_bytestring.size());
  std::memcpy(new_string.data(),
              reinterpret_cast<const uint8_t *>(input_bytestring.data()),
              input_bytestring.size());

  bool exclusive_lock =
      (flags & std::to_underlying(StorageDeltaModifier::kLockExclusive)) != 0;

  if (!is_valid_storage_delta_flag(flags)) {
    return SyscallError{SyscallErrorCode::kInvalidInput};
  }
  return item_view.set(BytestringItem(std::move(new_string), exclusive_lock));
}

SyscallStatus<std::pair<int128_t, int128_t>>
SystemCallHandler::get_nnint(StorageKey const &storage_key)
{
  auto &item_view = invocation_frame_stack_.get_item_view(storage_key);

  if (auto const *nnint_item = item_view.template get<NNIntItem>()) {
    return std::make_pair(nnint_item->base_value_, nnint_item->add_value_);
  }

  if (auto const *unmodified_item = item_view.template get<UnmodifiedItem>()) {
    if (!unmodified_item->base_item_) {
      return SyscallError{SyscallErrorCode::kStorageKeyNotExist};
    }
    if (unmodified_item->base_item_->type() !=
        StorageItemType::kNonnegativeInt) {
      return SyscallError{SyscallErrorCode::kInvalidInput};
    }
    return std::make_pair(unmodified_item->base_item_->nnint(), int128_t{0});
  }

  return SyscallError{SyscallErrorCode::kInvalidInput};
}

SyscallStatus<void>
SystemCallHandler::set_nnint(StorageKey const &storage_key, int128_t set_value,
                             int128_t add_value)
{
  auto &item_view = invocation_frame_stack_.get_item_view(storage_key);
  return item_view.set(NNIntItem(set_value, add_value));
}

SyscallStatus<void>
SystemCallHandler::retrocausal_queue_insert(
    StorageKey const &key, std::span<const std::byte> queue_entry)
{
  auto &item_view = invocation_frame_stack_.get_item_view(key);

  if (!item_view.can_accept_type(StorageItemType::kRetrocausalQueue)) {
    return SyscallError{SyscallErrorCode::kInvalidInput};
  }

  if (queue_entry.size() > kQueueEntryMaxLen) {
    return SyscallError{SyscallErrorCode::kInvalidInput};
  }

  QueueEntry new_entry;
  new_entry.resize_uninitialized(queue_entry.size());
  std::memcpy(new_entry.data(), queue_entry.data(), queue_entry.size());

  if (auto const *retrocausal_queue_item =
          item_view.template get<RetrocausalQueueItem>()) {
    RetrocausalQueueItem new_item = *retrocausal_queue_item;
    new_item.insert_entry(new_entry);
    return item_view.set(new_item);
  }

  auto const *unmodified_item = item_view.template get<UnmodifiedItem>();
  assert(unmodified_item);

  auto addrkey = invocation_frame_stack_.get_current_addrkey(key);

  RetrocausalQueueSnapshotCacheEntry const *queue = nullptr;
  if (unmodified_item->base_item_) {
    queue = &unmodified_item->base_item_->queue();
  }

  RetrocausalQueueItem new_item(queue, addrkey);

  new_item.insert_entry(new_entry);
  return item_view.set(new_item);
}

SyscallStatus<QueueEntry>
SystemCallHandler::retrocausal_queue_consume(StorageKey const &key, bool peek)
{
  auto &item_view = invocation_frame_stack_.get_item_view(key);
  auto addrkey = invocation_frame_stack_.get_current_addrkey(key);

  if (!item_view.can_accept_type(StorageItemType::kRetrocausalQueue)) {
    return SyscallError{SyscallErrorCode::kInvalidInput};
  }

  if (auto const *retrocausal_queue_item =
          item_view.template get<RetrocausalQueueItem>()) {
    RetrocausalQueueItem new_item = *retrocausal_queue_item;
    auto next_entry = new_item.get_next_item(execution_mode_data_);
    if (!next_entry) {
      return SyscallError{SyscallErrorCode::kNoQueueEntry};
    }

    if (!peek) {
      new_item.advance_cursor(execution_mode_data_);
    }

    [[maybe_unused]]
    auto set = item_view.set(new_item);
    assert(set);
    return SyscallStatus<QueueEntry>(*next_entry);
  }

  if (auto const *unmodified_item = item_view.template get<UnmodifiedItem>()) {
    if (!unmodified_item->base_item_) {
      return SyscallError{SyscallErrorCode::kStorageKeyNotExist};
    }
    // must be true because can_accept_type() returned true
    assert(unmodified_item->base_item_->type() ==
           StorageItemType::kRetrocausalQueue);

    RetrocausalQueueSnapshotCacheEntry const *queue = nullptr;
    if (unmodified_item->base_item_) {
      queue = &unmodified_item->base_item_->queue();
    }

    RetrocausalQueueItem new_item(queue, addrkey);

    auto next_entry = new_item.get_next_item(execution_mode_data_);
    if (!next_entry) {
      return SyscallError{SyscallErrorCode::kNoQueueEntry};
    }
    if (!peek) {
      new_item.advance_cursor(execution_mode_data_);
    }

    assert(new_item.validate());
    [[maybe_unused]]
    auto set = item_view.set(new_item);
    assert(set);

    return SyscallStatus<QueueEntry>(*next_entry);
  }

  assert(false);
  return SyscallError{SyscallErrorCode::kInvalidInput};
}

SyscallStatus<void>
SystemCallHandler::delete_object(StorageKey const &storage_key)
{
  auto &item_view = invocation_frame_stack_.get_item_view(storage_key);
  return item_view.set(DeletedItem{});
}

ProposalModeData const &
SystemCallHandler::get_proposal_data() const
{
  assert(std::holds_alternative<ProposalModeData>(execution_mode_data_));
  return std::get<ProposalModeData>(execution_mode_data_);
}

InvokeResult
SystemCallHandler::finish_nondeterminism_checks(InvokeResult const &res)
{
  if (std::holds_alternative<ValidationModeData>(execution_mode_data_)) {
    if (std::get<ValidationModeData>(execution_mode_data_)
            .check_all_consumed()) {
      // all proposal bits have been consumed, so validation succeeds.
      return res;
    }
    // received bad proposal data.
    return InvokeResult{.result_ =
                            InvokeError{InvokeErrorCode::kValidationFailure},
                        .gas_consumed_ = res.gas_consumed_};
  }
  return res;
}

} // namespace hogd
