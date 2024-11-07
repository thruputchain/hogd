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
#include "hogd/hogexec/runtime/auth_manager.h"
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
    root_auth_manager_(signed_tx_), snapshot_cache_(snapshot_cache)
{
  wasmi_context_.link_fn("marmot", "syscall", &syscall_entry);
}

InvokeResult
SystemCallHandler::root_invoke()
{
  if (!AuthManager::initialization_validate_auth_entries(signed_tx_)) {
    return InvokeResult{.result = InvokeError{InvokeErrorCode::kDeterministic},
                        .gas_consumed = 0};
  }

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

  start_call_frame(signed_tx_.tx.invocation, 0);

  auto res = runtime->runtime.invoke(signed_tx_.tx.invocation.method,
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

  total_gas_consumption_ += res.gas_consumed;

  auto const &auth_entries = root_auth_manager_.get_auth_entries();

  for (auto const &[authorizing_address, auth_context] : auth_entries) {
    if (!auth_context.auth_state_.has_been_verified()) {

      assert(total_gas_consumption_ <= signed_tx_.tx.invocation.gas_limit);

      uint64_t gas_remaining =
          signed_tx_.tx.invocation.gas_limit - total_gas_consumption_;

      // We don't care about the returned result here -- finish_call_frame takes
      // care of updating root_auth_manager_ with the result
      auto [_, local_gas_consumed] =
          invoke_verify_auth(authorizing_address, gas_remaining);

      if (integer_add_overflow(local_gas_consumed, total_gas_consumption_,
                               &total_gas_consumption_)) {
        return InvokeResult{.result = InvokeError{InvokeErrorCode::kOutOfGas},
                            .gas_consumed = UINT64_MAX};
      }

      if (total_gas_consumption_ > signed_tx_.tx.invocation.gas_limit) {
        return InvokeResult{.result = InvokeError{InvokeErrorCode::kOutOfGas},
                            .gas_consumed = total_gas_consumption_};
      }
    }
  }

  if (!root_auth_manager_.all_in_valid_termination_state()) {
    return InvokeResult{.result = InvokeError{InvokeErrorCode::kDeterministic},
                        .gas_consumed = total_gas_consumption_};
  }

  for (auto const &[addr, item] : root_item_cache_map_.local_map_) {
    auto deltas = item.get_deltas();

    if (!deltas.empty()) {
      committed_delta_vec_.emplace_back(addr, deltas);
    }
  }

  return InvokeResult{.result = 0, .gas_consumed = total_gas_consumption_};
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
      return {SyscallErrorCode::kDeterministicWasmError, result.gas_consumed};
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
            ->runtime.get_available_gas();
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

  start_call_frame(invocation, 0);

  auto res = runtime->runtime.invoke(invocation.method, *gas_limit);

  finish_call_frame(res);

  return convert_invoke_result_to_host_fn_result(res);
}

namespace {

Invocation
make_verify_auth_invocation(ContractAddress const &authorizing_address,
                            AuthEntry const &auth_entry, uint64_t gas_limit)
{
  Invocation invocation;
  invocation.invoked_address = authorizing_address;
  invocation.method.method_type(MethodSpecifierType::kReservedName);
  invocation.method.reserved_name() = "_verify_auth";
  invocation.gas_limit = gas_limit;

  // TODO(geoff): figure out a nice way to transfer an auth_entry into wasm
  VerifyAuthArgs auth_calldata;
  auth_calldata.auth_metadata = auth_entry.metadata;
  auth_calldata.witness_count = auth_entry.witnesses.size();

  invocation.calldata = x23::serialize(auth_calldata);
  return invocation;
}

} // namespace

std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
SystemCallHandler::invoke_verify_auth(
    ContractAddress const &authorizing_address,
    GasAmount call_specified_gas_limit)
{
  auto gas_limit = get_invoke_gas_limit(call_specified_gas_limit);

  if (!gas_limit) {
    return std::pair{gas_limit.error().code(), 0};
  }

  auto *runtime = resolve_runtime(authorizing_address);

  if (!runtime) {
    return std::pair{SyscallErrorCode::kContractLookupFailed, 0};
  }

  const AuthManager &auth_manager =
      (call_frame_stack_.empty()) ? root_auth_manager_
                                  : call_frame_stack_.back().auth_manager_;

  if (!auth_manager.has_entry_for(authorizing_address)) {
    return std::pair{SyscallErrorCode::kNoAuthRequired, 0};
  }

  if (!call_frame_stack_.empty()) {
    if (!std::holds_alternative<NormalFrameContext>(
            call_frame_stack_.back().extra_context_)) {
      return std::pair{SyscallErrorCode::kInvalidInvokeContext, 0};
    }
  }

  auto const &auth_entry = auth_manager.get_entry_for(authorizing_address);

  Invocation invocation =
      make_verify_auth_invocation(authorizing_address, auth_entry, *gas_limit);

  start_verify_auth_frame(invocation);

  auto res = runtime->runtime.invoke(invocation.method, *gas_limit);

  finish_call_frame(res);

  return convert_invoke_result_to_host_fn_result(res);
}

void
SystemCallHandler::start_verify_auth_frame(Invocation const &invocation)
{
  uint64_t new_flags = 0;

  if (call_frame_stack_.empty()) {
    call_frame_stack_.emplace_back(invocation, root_auth_manager_,
                                   AuthContextStack{}, new_flags,
                                   VerifyAuthFrameContext{
                                       .authorizing_address =
                                           invocation.invoked_address});
    return;
  }

  CallFrame new_frame(invocation, call_frame_stack_.back().auth_manager_,
                      call_frame_stack_.back().auth_context_stack_,
                      call_frame_stack_.back().flags_ | new_flags,
                      VerifyAuthFrameContext{.authorizing_address =
                                                 invocation.invoked_address});

  call_frame_stack_.emplace_back(std::move(new_frame));
}

void
SystemCallHandler::start_call_frame(Invocation const &invocation,
                                    uint64_t new_flags)
{
  if (call_frame_stack_.empty()) {
    call_frame_stack_.emplace_back(invocation, root_auth_manager_,
                                   AuthContextStack{}, new_flags,
                                   NormalFrameContext{});
    return;
  }

  // emplace_back might invalidate the reference to call_frame_stack_back(),
  // and I can't find where in the standard the precise point at which this
  // invalidation occurs (but it would make sense to do the realloc before a
  // placement new, so I assume it occurs before the ctor is called)
  CallFrame new_frame(invocation, call_frame_stack_.back().auth_manager_,
                      call_frame_stack_.back().auth_context_stack_,
                      call_frame_stack_.back().flags_ | new_flags,
                      call_frame_stack_.back().extra_context_);
  call_frame_stack_.emplace_back(std::move(new_frame));
}

void
SystemCallHandler::finish_call_frame(InvokeResult const &result)
{
  // Error handling: Each contract's return value is effectively a Result<Value,
  // i32>, or a trap(). A result return (through either the return syscall, the
  // method returning, or an Exit syscall) does not revert the function. Only a
  // deterministic trap() (such as out of gas, or divide by 0) cause a contract
  // to be reverted. We do not need a separate Revert() syscall -- a contract
  // can just call wasm `unreachable`.
  //
  // If a contract has the REVERT_SAFE flag set, then it can be called again
  // after it has been reverted. A contract might not be REVERT_SAFE because
  // reversion does NOT reset a wasm's internal memory or other internal state.
  // We could do that, it's just expensive,
  // and would require fairly close integration with the wasm runtime and some
  // fairly invasive changes (We need to restore the entirety of the wasm
  // module's internal state -- not just memory, but also tables and globals.
  // The stack is reset automatically).

  // If the frame is the root of a verify_auth invocation tree:
  // mark the error here, then commit.
  //
  // Verify_auth is special: The auth is marked as failed EVEN IF
  // the verify_auth call reverts.
  auto is_verify_auth_frame = [](CallFrame const &frame) -> bool {
    return std::holds_alternative<VerifyAuthFrameContext>(frame.extra_context_);
  };

  if (is_verify_auth_frame(call_frame_stack_.back())) {
    if (call_frame_stack_.size() == 1 ||
        is_verify_auth_frame(call_frame_stack_[call_frame_stack_.size() - 2])) {

      auto &parent_auth_manager =
          (call_frame_stack_.size() == 1)
              ? root_auth_manager_
              : call_frame_stack_[call_frame_stack_.size() - 2].auth_manager_;

      if (result.result.has_value() && *result.result == 0) {
        parent_auth_manager.mark_verify_auth_successful(
            call_frame_stack_.back().active_invocation_.invoked_address);
      }
      else {
        parent_auth_manager.mark_verify_auth_failed(
            call_frame_stack_.back().active_invocation_.invoked_address);
      }
    }
  }

  // Revert the function
  if (!result.result.has_value()) {

    for (auto const &addr : call_frame_stack_.back().visited_runtimes_) {
      // TODO(geoff) metadata allowing revert_safe declarations
      runtimes_.at(addr)->revert_poisoned = true;
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

void
SystemCallHandler::commit_last_call_frame_success(CallFrame &frame)
{
  auto logs = frame.extract_logs();

  // Appends to logs, instead of overwrite.
  // Required for the case where verify_auth starts more invocations after the
  // last main invocation finishes
  committed_logs_.insert(committed_logs_.end(), logs.begin(), logs.end());

  root_item_cache_map_.commit_child_map(frame.get_item_cache_map());

  root_auth_manager_ = std::move(frame.auth_manager_);
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

    if (out->revert_poisoned) {
      return nullptr;
    }

    return out;
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
namespace {

AuthContext
make_auth_context(ContractAddress const &addr,
                  std::span<const std::byte> action)
{

  AuthContext new_context;
  new_context.address = addr;

  std::span<const uint8_t> reinterpreted_auth_frame = {
      reinterpret_cast<const uint8_t *>(action.data()), action.size()};
  new_context.bytes = {reinterpreted_auth_frame.begin(),
                       reinterpreted_auth_frame.end()};

  return new_context;
}

} // namespace

void
SystemCallHandler::push_auth_context(
    std::span<const std::byte> auth_context_frame)
{
  auto &frame = get_active_call_frame();
  frame.auth_context_stack_.context_.push_back(
      make_auth_context(frame.active_invocation_.invoked_address,
                        auth_context_frame));
}

SyscallStatus<void>
SystemCallHandler::pop_auth_context()
{
  auto &frame = get_active_call_frame();
  if (frame.auth_context_stack_.can_pop_context()) {
    frame.auth_context_stack_.context_.pop_back();
    return {};
  }
  return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
}

SyscallStatus<void>
SystemCallHandler::require_auth(
    const ContractAddress &authorizing_address,
    std::optional<std::span<const std::byte>> action)
{
  auto &active_frame = get_active_call_frame();

  if (action.has_value()) {
    auto leaf_context = make_auth_context(authorizing_address, *action);
    active_frame.auth_context_stack_.context_.push_back(leaf_context);
  }
  else {
    auto &calldata = active_frame.active_invocation_.calldata;
    auto full_auth_context =
        x23::serialize(active_frame.active_invocation_.method, calldata);
    auto leaf_context =
        make_auth_context(authorizing_address,
                          std::as_bytes(std::span(full_auth_context)));
    active_frame.auth_context_stack_.context_.push_back(leaf_context);
  }
  x23::Defer d([&] { active_frame.auth_context_stack_.context_.pop_back(); });

  if (!active_frame.auth_manager_.try_match(authorizing_address,
                                            active_frame.auth_context_stack_)) {
    return SyscallError{SyscallErrorCode::kAuthMatchFailed};
  }

  return {};
}

bool
SystemCallHandler::inside_verify_auth_frame() const
{
  assert(!call_frame_stack_.empty());

  return std::holds_alternative<VerifyAuthFrameContext>(
      call_frame_stack_.back().extra_context_);
}

SyscallStatus<Witness>
SystemCallHandler::get_auth_witness(uint32_t wit_index) const
{
  if (!inside_verify_auth_frame()) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  auto const &authorizing_address =
      std::get<VerifyAuthFrameContext>(call_frame_stack_.back().extra_context_)
          .authorizing_address;

  auto const &entry =
      call_frame_stack_.back().auth_manager_.get_entry_for(authorizing_address);

  if (entry.witnesses.size() <= wit_index) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  return entry.witnesses[wit_index];
}

SyscallStatus<AuthEntrySignaturePayload>
SystemCallHandler::get_auth_payload(const SHA256Hash *optional_input_message)
{
  assert(!call_frame_stack_.empty());

  if (!inside_verify_auth_frame()) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }

  AuthEntrySignaturePayload payload;
  if (optional_input_message) {
    payload.message = *optional_input_message;
  }
  else {
    payload.message = get_source_tx_hash();
  }

  payload.authorizing_address =
      std::get<VerifyAuthFrameContext>(call_frame_stack_.back().extra_context_)
          .authorizing_address;

  if (!root_auth_manager_.has_entry_for(payload.authorizing_address)) {
    return SyscallError{SyscallErrorCode::kInvalidSyscallInput};
  }
  auto const &entry =
      root_auth_manager_.get_entry_for(payload.authorizing_address);

  payload.nodes = entry.nodes;
  payload.metadata = entry.metadata;

  return payload;
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
