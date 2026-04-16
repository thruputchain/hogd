#include "hogd/hogexec/runtime/invocation_frame.h"

#include <cassert>
#include <iterator>
#include <optional>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

#include "contract.hh"
#include "event.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/item_view.h"
#include "invoke_flag.hh"
#include "storage_item.hh"
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
InvocationFrame::commit_manually_reverted_frame(InvocationFrame &&frame)
{
  assert(frame.should_revert());
  child_return_output_ = std::move(frame.to_return_output_);
}

void
InvocationFrame::commit_successful_child_frame(InvocationFrame &&frame)
{
  child_return_output_ = std::move(frame.to_return_output_);

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
}

void
InvocationFrame::log_event(TxEmittedEvent event)
{
  events_.emplace_back(std::move(event));
}

void
InvocationFrame::set_frame_return(InvocationFrameOutput &&return_output)
{
  to_return_output_ = std::move(return_output);
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

SystemStatus<bool>
InvocationFrame::try_acquire_frame_locks(PerTransactionDatabaseProxy &db_proxy)
{
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
InvocationFrameStack::start_frame(ContractAddress const &address,
                                  Calldata &&calldata, InvokeFlagSet flags)
{
  frame_stack_.emplace_back(address, std::forward<Calldata>(calldata), flags);
}

size_t
InvocationFrameStack::event_count() const
{
  size_t total = 0;
  for (auto const &frame : frame_stack_) {
    total += frame.get_events().size();
  }
  return total;
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

      // If we're in proposal mode, we need to acquire the frame locks.
      if (std::holds_alternative<ProposalModeData>(execution_mode_data)) {
        auto acq_res = frame_stack_.back().try_acquire_frame_locks(db_proxy_);
        if (!acq_res.has_value()) {
          frame_stack_.pop_back();
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
          frame_stack_.pop_back();
          return SystemError{SystemErrorCode::ValidationFailure};
        }
        acq_success = res.value().get();
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

} // namespace hogd
