#include "hogd/hogexec/runtime/invocation_frame.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "auth_token.hh"
#include "contract.hh"
#include "event.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/auth_token_registry.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/item_view.h"
#include "invoke_flag.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

InvocationFrame::InvocationFrame(ContractAddress const &address,
                                 Calldata &&calldata, InvokeFlagSet flags,
                                 size_t event_count,
                                 std::span<uint32_t const> owned_auth_token_ids,
                                 uint32_t inherited_try_invoke_depth)
  : address_(address), calldata_(std::forward<Calldata>(calldata)),
    flags_(flags), event_count_(event_count),
    total_try_invoke_depth_(inherited_try_invoke_depth),
    owned_auth_token_ids_(owned_auth_token_ids.begin(),
                          owned_auth_token_ids.end())
{}

void
InvocationFrame::commit_manually_reverted_frame(InvocationFrame &&frame)
{
  assert(frame.should_revert());
  child_return_output_ = std::move(frame.to_return_output_);
  child_returned_auth_token_ids_.clear();

  // reclaim the auth tokens from a reverted frame
  for (auto token_id : most_recent_child_frame_forwarded_auth_tokens_) {
    owned_auth_token_ids_.insert(token_id);
  }
  most_recent_child_frame_forwarded_auth_tokens_.clear();
}

void
InvocationFrame::commit_successful_child_frame(InvocationFrame &&frame)
{
  child_return_output_ = std::move(frame.to_return_output_);
  event_count_ = frame.event_count_;

  events_.reserve(events_.size() + frame.events_.size());
  events_.append_range(
      std::ranges::subrange(std::make_move_iterator(frame.events_.begin()),
                            std::make_move_iterator(frame.events_.end())));

  new_contract_deployments_.reserve(new_contract_deployments_.size() +
                                    frame.new_contract_deployments_.size());
  for (auto const &[addr, hash] : frame.new_contract_deployments_) {
    [[maybe_unused]] auto [_, inserted_new] =
        new_contract_deployments_.emplace(addr, hash);
    assert(inserted_new);
  }

  // transfer modified items from child frame to current frame
  modified_items_.reserve(modified_items_.size() +
                          frame.modified_items_.size());
  for (auto &[addr, item] : frame.modified_items_) {
    modified_items_.insert_or_assign(addr, std::move(item));
  }

  try_invoke_frame_count_ += frame.try_invoke_frame_count_;
  total_try_invoke_depth_ = frame.total_try_invoke_depth_;

  // Reclaim auth tokens the child returned via the Return syscall
  for (auto token_id : frame.returned_auth_token_ids_) {
    owned_auth_token_ids_.insert(token_id);
  }
  child_returned_auth_token_ids_ = std::move(frame.returned_auth_token_ids_);

  most_recent_child_frame_forwarded_auth_tokens_.clear();
}

SystemStatus<void>
InvocationFrame::mark_frame_as_failed(
    PerTransactionDatabaseProxy &db_proxy) const
{
  if (try_invoke_frame_count_ > 0) {
    auto st = db_proxy.pop_subtransactions(try_invoke_frame_count_);
    if (!st.has_value()) {
      return st.error();
    }
  }
  return {};
}

void
InvocationFrame::commit_failed_child_frame()
{
  child_return_output_ =
      InvocationFrameOutput{InvocationFrameStatusCode::Fault};
  child_returned_auth_token_ids_.clear();
  // reclaim the auth tokens from a failed frame
  for (auto token_id : most_recent_child_frame_forwarded_auth_tokens_) {
    owned_auth_token_ids_.insert(token_id);
  }
  most_recent_child_frame_forwarded_auth_tokens_.clear();
}

void
InvocationFrame::log_event(TxEmittedEvent event)
{
  event_count_++;
  events_.emplace_back(std::move(event));
}

void
InvocationFrame::set_frame_return(
    InvocationFrameOutput &&return_output,
    std::vector<uint32_t> &&returned_auth_token_ids)
{
  to_return_output_ = std::move(return_output);
  // only return auth tokens in frame success
  assert(returned_auth_token_ids.empty() ||
         to_return_output_.status_code() == InvocationFrameStatusCode::Return);
  returned_auth_token_ids_ = std::move(returned_auth_token_ids);
}

namespace {

StorageDelta
make_deployed_contract_delta(DeployedContractHash const &hash)
{
  StorageDelta delta = StorageDelta(StorageDeltaType::kDeltaTypeDeployContract);
  delta.deployed_contract_hash() = hash;
  return delta;
}

} // namespace

void
InvocationFrame::note_validation_try_invoke_success()
{
  assert(!depth_cap_reached());
  total_try_invoke_depth_++;
}

SystemStatus<bool>
InvocationFrame::try_acquire_frame_locks(PerTransactionDatabaseProxy &db_proxy)
{
  // Pushes the accumulated delta batch as a HogDB subtransaction. Returns
  // true on a successful lock acquisition, false on contention failure.
  // On success, increments the per-frame TryInvoke counters; the root commit
  // caller (SystemCallHandler::try_commit_transaction) ignores those counters,
  // since the root frame is discarded immediately after commit.
  //
  // The deterministic TryInvoke depth cap (kMaxSavepointDepth) is NOT enforced
  // here; it is enforced exclusively by InvocationFrameStack::finish_frame's
  // depth-cap arm before this is called for a child TryInvoke frame. The root
  // commit caller has no TryInvoke semantics, so the cap does not apply there.

  db_proxy.reserve(modified_items_.size() + new_contract_deployments_.size());

  for (auto const &[addrkey, item_view] : modified_items_) {
    auto [delta, flags] = item_view.get_delta();
    if (delta) {
      db_proxy.add_delta(addrkey, std::move(*delta), flags);
    }
  }

  for (auto const &[addr, deployment] : new_contract_deployments_) {
    AddressAndKey addrkey = {.address = addr,
                             .key = StorageKey(StorageKeyType::Metadata)};
    db_proxy.add_delta(addrkey, make_deployed_contract_delta(deployment),
                       std::to_underlying(StorageDeltaFlags::None));
  }

  auto res = db_proxy.try_push_current_subtransaction();
  if (!res.has_value()) {
    return res.error();
  }
  if (res.value()) {
    try_invoke_frame_count_++;
    total_try_invoke_depth_++;
  }
  return res.value();
}

void
InvocationFrame::collect_storage_deltas(
    x23::Vec<FlaggedStorageDelta, 0xffffffff> &out) const
{
  out.reserve(out.size() + modified_items_.size() +
              new_contract_deployments_.size());
  for (auto const &[addrkey, item_view] : modified_items_) {
    auto [delta, flags] = item_view.get_delta();
    if (delta) {
      out.push_back(FlaggedStorageDelta{
          .addrkey = addrkey,
          .delta = *delta,
          .flags = flags,
      });
    }
  }

  for (auto const &[addr, deployment] : new_contract_deployments_) {
    AddressAndKey addrkey = {.address = addr,
                             .key = StorageKey(StorageKeyType::Metadata)};
    StorageDelta delta = make_deployed_contract_delta(deployment);
    out.push_back(FlaggedStorageDelta{
        .addrkey = addrkey,
        .delta = delta,
        .flags = std::to_underlying(StorageDeltaFlags::None),
    });
  }
}

void
InvocationFrameStack::start_frame(
    ContractAddress const &address, Calldata &&calldata, InvokeFlagSet flags,
    std::span<uint32_t const> owned_auth_token_ids)
{
  assert(frame_stack_.empty() || check_auth_tokens_owned(owned_auth_token_ids));

  size_t event_count =
      frame_stack_.empty() ? 0 : frame_stack_.back().event_count();
  if (!frame_stack_.empty()) {
    frame_stack_.back().remove_auth_tokens(owned_auth_token_ids);
    frame_stack_.back().set_child_restore_auth_tokens(owned_auth_token_ids);
  }
  uint32_t parent_depth =
      frame_stack_.empty() ? 0
                           : frame_stack_.back().get_total_try_invoke_depth();
  frame_stack_.emplace_back(address, std::forward<Calldata>(calldata), flags,
                            event_count, owned_auth_token_ids, parent_depth);
}

SystemStatus<std::pair<InvokeResult, bool>>
InvocationFrameStack::finish_frame(InvokeStatus<void> result,
                                   ExecutionModeData &execution_mode_data)
{
  InvokeResult frame_result = {
      .result_ = result,
      .gas_consumed_ = 0,
  };

  [[maybe_unused]] bool try_invoke_failed = false;

  // handle the try_invoke data
  assert(!frame_stack_.empty());
  if (frame_stack_.back().is_try_invoke()) {
    // Only relevant in the case where the deterministic part of the frame
    // succeeds.
    if (frame_result.is_frame_success()) {
      bool acq_success = false;

      if (frame_stack_.back().depth_cap_reached()) {
        // Deterministic failure across both proposal and validation modes.
        // No nondeterministic bit is logged or consumed here, so the bit
        // stream represents only lock-acquisition outcomes.
        acq_success = false;
      }
      else if (std::holds_alternative<ProposalModeData>(execution_mode_data)) {
        auto acq_res = frame_stack_.back().try_acquire_frame_locks(db_proxy_);
        if (!acq_res.has_value()) {
          frame_stack_.pop_back();
          return acq_res.error();
        }
        acq_success = acq_res.value();

        std::get<ProposalModeData>(execution_mode_data)
            .log_nondeterministic_bit(acq_success);
      }
      else if (std::holds_alternative<ViewingModeData>(execution_mode_data)) {
        auto bit = std::get<ViewingModeData>(execution_mode_data)
                       .view_try_invoke_bit();
        acq_success = bit.get();
      }
      else {
        auto res = std::get<ValidationModeData>(execution_mode_data)
                       .consume_nondeterministic_bit();
        if (!res.has_value()) {
          frame_stack_.pop_back();
          return SystemError{SystemErrorCode::ValidationFailure};
        }
        acq_success = res.value().get();
        if (acq_success) {
          frame_stack_.back().note_validation_try_invoke_success();
        }
      }

      // If the frame locks were not acquired successfully, then we mark
      // the entire frame as failing.
      if (!acq_success) {
        try_invoke_failed = true;
        // TODO(geoff): perhaps add additional gas consumption
        frame_result = InvokeResult{
            .result_ = InvokeError{InvokeErrorCode::Deterministic},
            .gas_consumed_ = 0,
        };
      }
    }
    // otherwise nothing to do here with try_invoke
  }

  if (frame_result.should_frame_revert() ||
      frame_stack_.back().should_revert()) {
    auto st = frame_stack_.back().mark_frame_as_failed(db_proxy_);
    if (!st.has_value()) {
      frame_stack_.pop_back();
      return st.error();
    }
  }

  bool manually_reverted = frame_stack_.back().should_revert();

  if (frame_result.should_frame_revert()) {
    assert(try_invoke_failed ||
           frame_stack_.back().get_frame_output().status_code() ==
               InvocationFrameStatusCode::Fault);
    frame_stack_.pop_back();

    if (!frame_stack_.empty()) {
      frame_stack_.back().commit_failed_child_frame();
    }

    return {{frame_result, manually_reverted}};
  }

  // commit child invocation frame successfully
  assert(frame_result.is_frame_success());

  size_t stack_size = frame_stack_.size();
  if (stack_size >= 2) {
    // Commit the frame to it's parent
    // Accounts for whether or not the frame is set to manually revert
    if (frame_stack_.back().should_revert()) {
      frame_stack_[stack_size - 2].commit_manually_reverted_frame(
          std::move(frame_stack_.back()));
    }
    else {
      frame_stack_[stack_size - 2].commit_successful_child_frame(
          std::move(frame_stack_.back()));
    }
  }
  else {
    assert(stack_size == 1);
    // last frame -- push frame contents to external caller
    // (SystemCallHandler), via the supplied lambda
    last_frame_action_(std::move(frame_stack_.back()));
  }

  frame_stack_.pop_back();

  return {{frame_result, manually_reverted}};
}

void
InvocationFrameStack::log_trace_finish(InvocationFrameTrace &trace)
{
  assert(!frame_stack_.empty());
  frame_stack_.back().collect_storage_deltas(trace.storage_writes);
  trace.status = frame_stack_.back().get_frame_output();
}

std::optional<ContractAddress>
InvocationFrameStack::get_msg_sender() const
{
  return frame_stack_.size() >= 2
             ? std::optional<ContractAddress>{frame_stack_[frame_stack_.size() -
                                                           2]
                                                  .address()}
             : std::nullopt;
}

std::optional<DeployedContractHash>
InvocationFrameStack::lookup_locally_deployed_contract(
    ContractAddress const &addr) const
{
  for (auto const &frame : std::ranges::reverse_view(frame_stack_)) {
    if (auto out = frame.lookup_locally_deployed_contract(addr)) {
      assert(out.has_value());
      return out;
    }
  }
  return std::nullopt;
}

ItemView *
InvocationFrame::add_item_view(AddressAndKey const &addrkey, ItemView new_view)
{
  assert(!modified_items_.contains(addrkey));
  auto [iter, _] = modified_items_.emplace(addrkey, std::move(new_view));
  return &iter->second;
}

ItemView *
InvocationFrameStack::get_item_view(AddressAndKey const &addrkey)
{
  const int32_t stack_depth = frame_stack_.size();
  assert(stack_depth > 0);

  auto &active_frame = frame_stack_.back();

  // do we already have a frame in the top active frame
  if (auto *active_frame_view = active_frame.get_item_view(addrkey)) {
    return active_frame_view;
  }

  // otherwise create view in active frame, by either copying from
  // a parent call frame or from storage
  for (int32_t i = stack_depth - 2; i >= 0; --i) {
    auto &frame = frame_stack_[i];

    if (auto const *local_view = frame.get_item_view(addrkey)) {
      // refresh the parent frame view in the top frame
      return active_frame.add_item_view(addrkey, *local_view);
    }
  }

  return frame_stack_.back().add_item_view(addrkey, ItemView());
}

ItemView *
InvocationFrameStack::get_item_view(StorageKeyContractData const &data_key)
{
  return get_item_view(get_addrkey(data_key));
}

AuthToken const *
InvocationFrameStack::lookup_auth_token(uint32_t token_id) const
{
  assert(!frame_stack_.empty());
  if (frame_stack_.back().check_owns_auth_token(token_id)) {
    auto const *out = auth_token_registry_.lookup_auth_token(token_id);
    CHECK_CONST_PTR(out);
    return out;
  }
  return nullptr;
}

SyscallStatus<uint32_t>
InvocationFrameStack::mint_auth_token(AuthToken const &auth_token)
{
  if (frame_stack_.empty()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  auto token_id = auth_token_registry_.register_auth_token(auth_token);
  if (!token_id.has_value()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  frame_stack_.back().mint_auth_token(token_id.value());
  return token_id.value();
}

SyscallStatus<InvocationFrameStack::ConsumeAuthTokenResult>
InvocationFrameStack::consume_auth_token(uint32_t token_id)
{
  assert(!frame_stack_.empty());
  if (!frame_stack_.back().check_owns_auth_token(token_id)) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  auto const *token = auth_token_registry_.lookup_auth_token(token_id);
  CHECK_CONST_PTR(token);

  if (token->root_node.consumer_address != frame_stack_.back().address()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto unwrap_result = auth_token_registry_.unwrap_auth_token(token_id);
  if (!unwrap_result.has_value()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  for (uint32_t i = 0; i < unwrap_result->new_token_id_count; i++) {
    frame_stack_.back().mint_auth_token(unwrap_result->new_token_id_start + i);
  }

  frame_stack_.back().remove_auth_tokens(
      std::span<uint32_t const>{&token_id, 1});

  return ConsumeAuthTokenResult{.payload = &token->root_node.payload,
                                .mint_address = &token->mint_address,
                                .new_token_id_start =
                                    unwrap_result->new_token_id_start,
                                .new_token_id_count =
                                    unwrap_result->new_token_id_count};
}

bool
InvocationFrameStack::check_auth_tokens_owned(
    std::span<uint32_t const> token_ids) const
{
  assert(!frame_stack_.empty());
  return std::ranges::all_of(token_ids, [&](uint32_t token_id) {
    return frame_stack_.back().check_owns_auth_token(token_id);
  });
}

} // namespace hogd
