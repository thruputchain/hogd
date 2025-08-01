#include "hogd/hogexec/runtime/invocation_frame.h"

#include <cassert>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/status.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

InvocationFrame::InvocationFrame(ContractAddress const &address,
                                 Calldata &&calldata, InvokeFlagSet flags)
  : address_(address), calldata_(std::forward<Calldata>(calldata)),
    flags_(flags)
{}

void
InvocationFrame::commit_successful_child_frame(InvocationFrame &&frame)
{
  child_return_buffer_ = frame.to_return_;
  logs_.insert(logs_.end(), std::make_move_iterator(frame.logs_.begin()),
               std::make_move_iterator(frame.logs_.end()));

  for (auto const &[addr, hash] : frame.new_contract_deployments_) {
    [[maybe_unused]] auto [_, inserted_new] =
        new_contract_deployments_.emplace(addr, hash);
    assert(inserted_new);
  }

  // transfer modified items from child frame to current frame
  for (auto &[addr, item] : frame.modified_items_) {
    modified_items_.erase(addr);
    modified_items_.emplace(addr, std::move(item));
  }

  try_invoke_frame_count_ += frame.try_invoke_frame_count_;
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
  child_return_buffer_ = std::nullopt;
}

void
InvocationFrame::log(std::span<const std::byte> message)
{
  std::span<const uint8_t> m = {reinterpret_cast<const uint8_t *>(
                                    message.data()),
                                message.size()};
  logs_.emplace_back(address_,
                     x23::Vec<uint8_t, kMaxLogMessageLen>(m.begin(), m.end()));
}

void
InvocationFrame::set_frame_return(ReturnBuffer &&return_buffer)
{
  to_return_ = std::move(return_buffer);
}

SystemStatus<bool>
InvocationFrame::try_acquire_frame_locks(PerTransactionDatabaseProxy &db_proxy)
{

  for (auto const &[addrkey, item_view] : modified_items_) {
    auto delta = item_view.get_delta();
    if (delta) {
      // TODO(geoff): exclusive write flag not implemented yet
      db_proxy.add_delta(addrkey, *delta,
                         std::to_underlying(StorageItemRPCFlags::kStorageNone));
    }
  }

  auto res = db_proxy.try_push_current_subtransaction();
  if (!res.has_value()) {
    return res.error();
  }
  if (res.value()) {
    try_invoke_frame_count_++;
  }
  return res.value();
}

void
InvocationFrameStack::start_frame(ContractAddress const &address,
                                  Calldata &&calldata, InvokeFlagSet flags)
{
  frame_stack_.emplace_back(address, std::forward<Calldata>(calldata), flags);
}

SystemStatus<InvokeResult>
InvocationFrameStack::finish_frame(InvokeResult result,
                                   ExecutionModeData &execution_mode_data)
{
  // handle the try_invoke data
  assert(!frame_stack_.empty());
  if (frame_stack_.back().is_try_invoke()) {
    // Only relevant in the case where the deterministic part of the frame
    // succeeds.
    if (result.is_frame_success()) {
      bool acq_success = false;

      // If we're in proposal mode, we need to acquire the frame locks.
      if (std::holds_alternative<ProposalModeData>(execution_mode_data)) {
        auto acq_res = frame_stack_.back().try_acquire_frame_locks(db_proxy_);
        if (!acq_res.has_value()) {
          return acq_res.error();
        }
        acq_success = acq_res.value();

        std::get<ProposalModeData>(execution_mode_data)
            .log_nondeterministic_bit(acq_success);
      }
      else {
        // Otherwise, we're in validation mode, so we read the nondeterministic
        // bit from the validation metadata.
        auto res = std::get<ValidationModeData>(execution_mode_data)
                       .consume_nondeterministic_bit();
        if (!res.has_value()) {
          return SystemError{SystemErrorCode::kValidationFailure};
        }
        acq_success = res.value().get();
      }

      // If the frame locks were not acquired successfully, then we mark
      // the entire frame as failing.
      if (!acq_success) {
        // TODO(geoff): perhaps add additional gas consumption
        result = InvokeResult{
            .result_ = InvokeError{InvokeErrorCode::kInvokeDeterministic},
            .gas_consumed_ = result.gas_consumed_,
        };
      }
    }
    // otherwise nothing to do here with try_invoke
  }

  if (result.should_frame_revert()) {
    auto st = frame_stack_.back().mark_frame_as_failed(db_proxy_);
    if (!st.has_value()) {
      return st.error();
    }
    frame_stack_.pop_back();

    if (!frame_stack_.empty()) {
      frame_stack_.back().commit_failed_child_frame();
    }

    return result;
  }

  // commit child invocation frame successfully
  assert(result.is_frame_success());

  size_t stack_size = frame_stack_.size();
  if (stack_size >= 2) {
    // Commit the frame to it's parent
    frame_stack_[stack_size - 2].commit_successful_child_frame(
        std::move(frame_stack_.back()));
  }
  else if (stack_size == 1) {
    // last frame -- push frame contents to external caller
    // (SystemCallHandler), via the supplied lambda
    last_frame_action_(std::move(frame_stack_.back()));
  }

  frame_stack_.pop_back();

  return result;
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

std::optional<SHA256Hash>
InvocationFrameStack::lookup_locally_deployed_contract(
    ContractAddress const &addr) const
{
  for (auto const &frame : frame_stack_) {
    auto it = frame.new_contract_deployments_.find(addr);
    if (it != frame.new_contract_deployments_.end()) {
      // contract was deployed at `addr` during a frame earlier on the frame
      // stack. Thus, deployment exists during the current view.
      return it->second;
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

} // namespace hogd
