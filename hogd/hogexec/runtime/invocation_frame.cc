#include "hogd/hogexec/runtime/invocation_frame.h"

#include <cassert>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/runtime_cache.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

InvocationFrame::InvocationFrame(Invocation invocation,
                                 RuntimeCacheEntry *visited_entry)
  : active_invocation_(std::move(invocation))
{
  visited_runtimes_.insert(visited_entry);
}

void
InvocationFrame::commit_successful_child_frame(InvocationFrame &frame)
{
  visited_runtimes_.insert(frame.visited_runtimes_.begin(),
                           frame.visited_runtimes_.end());

  child_return_buffer_ = frame.to_return_;
  logs_.insert(logs_.end(), std::make_move_iterator(frame.logs_.begin()),
               std::make_move_iterator(frame.logs_.end()));

  for (auto const &[addr, hash] : frame.new_contract_deployments_) {
    [[maybe_unused]] auto [_, inserted_new] =
        new_contract_deployments_.emplace(addr, hash);
    assert(inserted_new);
  }

  for (auto &[addr, item] : frame.modified_items_) {
    modified_items_.erase(addr);
    modified_items_.emplace(addr, std::move(item));
  }
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
  logs_.emplace_back(active_invocation_.invoked_address,
                     x23::Vec<uint8_t, kMaxLogMessageLen>(m.begin(), m.end()));
}

void
InvocationFrame::set_frame_return(ReturnBuffer &&return_buffer)
{
  to_return_ = std::move(return_buffer);
}

void
InvocationFrame::poison_visited_runtimes()
{
  for (auto *runtime : visited_runtimes_) {
    // TODO(geoff) metadata allowing revert_safe declarations
    runtime->revert_poisoned_ = true;
  }
}

void
InvocationFrameStack::start_frame(Invocation const &invocation,
                                  RuntimeCacheEntry *visited_entry)
{
  frame_stack_.emplace_back(invocation, visited_entry);
}

bool
InvocationFrame::handle_try_acquire_locks(PerTransactionDatabaseProxy &db_proxy)
{
  assert(is_try_invoke_frame());
  for (auto &[addr, item] : modified_items_) {
    auto delta = item.get_delta();
    if (delta) {
      auto res =
          db_proxy.try_reserve_delta(addr, (*delta),
                                     std::to_underlying(RPCFlags::kNone))();
      if (!res) {
        return false;
      }
    }
  }

  return true;
}

void
InvocationFrameStack::revert_frame([[maybe_unused]] InvokeResult const &result)
{
  assert(result.should_frame_revert());

  assert(!frame_stack_.empty());

  frame_stack_.back().poison_visited_runtimes();
  frame_stack_.pop_back();

  if (!frame_stack_.empty()) {
    frame_stack_.back().commit_failed_child_frame();
  }
}

InvokeResult
InvocationFrameStack::handle_try_invoke(InvokeResult const &prev_result,
                                        ExecutionModeData &execution_mode_data)
{
  assert(!frame_stack_.empty());
  if (!frame_stack_.back().is_try_invoke_frame()) {
    return prev_result;
  }
  if (std::holds_alternative<ProposalModeData>(execution_mode_data)) {
    auto &proposal_mode_data = std::get<ProposalModeData>(execution_mode_data);
    bool res = frame_stack_.back().handle_try_acquire_locks(db_proxy_);

    proposal_mode_data.log_try_invoke_bit(res);

    if (!res) {
      return InvokeResult{.result_ =
                              InvokeError{InvokeErrorCode::kDeterministic},
                          .gas_consumed_ = prev_result.gas_consumed_};
    }

    return prev_result;
  }

  assert(std::holds_alternative<ValidationModeData>(execution_mode_data));
  auto &validation_mode_data =
      std::get<ValidationModeData>(execution_mode_data);
  std::optional<bool> res = validation_mode_data.consume_try_invoke_bit();
  if (!res.has_value()) {
    return InvokeResult{.result_ =
                            InvokeError{InvokeErrorCode::kValidationFailure},
                        .gas_consumed_ = prev_result.gas_consumed_};
  }
  // otherwise, we got a bit from try_invoke replay, but it was false
  if (!res.value()) {
    return InvokeResult{.result_ = InvokeError{InvokeErrorCode::kDeterministic},
                        .gas_consumed_ = prev_result.gas_consumed_};
  }

  return prev_result;
}

InvokeResult
InvocationFrameStack::finish_frame(InvokeResult const &result,
                                   ExecutionModeData &execution_mode_data)
{
  // If a contract has the REVERT_SAFE flag set, then it can be called again
  // after it has been reverted. A contract might not be REVERT_SAFE because
  // reversion does NOT reset a wasm's internal memory or other internal state.
  // We could do that, it's just expensive,
  // and would require fairly close integration with the wasm runtime and some
  // fairly invasive changes (We need to restore the entirety of the wasm
  // module's internal state -- not just memory, but also tables and globals.
  // The wasm stack is reset automatically).
  if (result.should_frame_revert()) {
    revert_frame(result);
    return result;
  }

  InvokeResult post_try_invoke_result =
      handle_try_invoke(result, execution_mode_data);

  if (post_try_invoke_result.should_frame_revert()) {
    revert_frame(post_try_invoke_result);
    return post_try_invoke_result;
  }

  // commit child invocation frame successfully
  assert(post_try_invoke_result.is_frame_success());

  size_t stack_size = frame_stack_.size();
  if (stack_size >= 2) {
    // Commit the frame to it's parent
    frame_stack_[stack_size - 2].commit_successful_child_frame(
        frame_stack_.back());
  }
  else if (stack_size == 1) {
    // last frame can't be a try_invoke_frame
    assert(!frame_stack_.back().is_try_invoke_frame());
    // last frame -- push frame contents to external caller
    // (SystemCallHandler), via the supplied lambda
    last_frame_action_(frame_stack_.back());
  }

  frame_stack_.pop_back();
  return post_try_invoke_result;
}

std::optional<ContractAddress>
InvocationFrameStack::get_msg_sender() const
{
  return frame_stack_.size() >= 2
             ? std::optional<
                   ContractAddress>{frame_stack_[frame_stack_.size() - 2]
                                        .active_invocation_.invoked_address}
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

std::unordered_map<AddressAndKey, FlaggedStorageDelta>
InvocationFrame::extract_deltas()
{
  std::unordered_map<AddressAndKey, FlaggedStorageDelta> out;
  for (auto const &[k, v] : modified_items_) {
    auto d = v.get_delta();
    if (d) {
      out.emplace(k, *d);
    }
  }
  return out;
}

ItemView *
InvocationFrame::get_item_view(AddressAndKey const &addrkey)
{
  auto it = modified_items_.find(addrkey);

  if (it != modified_items_.end()) {
    return &(it->second);
  }

  return nullptr;
}

void
InvocationFrame::add_item_view(AddressAndKey const &addrkey, ItemView new_view)
{
  assert(modified_items_.find(addrkey) == modified_items_.end());
  modified_items_.emplace(addrkey, std::move(new_view));
}

ItemView &
InvocationFrameStack::get_item_view(StorageKey const &key)
{
  const int32_t stack_depth = frame_stack_.size();
  assert(stack_depth > 0);

  auto &active_frame = get_active_frame();
  AddressAndKey addrkey{.address =
                            active_frame.active_invocation_.invoked_address,
                        .key = key};

  // do we already have a frame in the top active frame
  if (auto *active_frame_view = active_frame.get_item_view(addrkey)) {
    return *active_frame_view;
  }

  // otherwise create view in active frame, by either copying from
  // a parent call frame or from storage
  for (int32_t i = stack_depth - 2; i >= 0; --i) {
    auto &frame = frame_stack_[i];

    if (auto const *local_view = frame.get_item_view(addrkey)) {
      active_frame.add_item_view(addrkey, *local_view);
      return *active_frame.get_item_view(addrkey);
    }
  }

  // item view not yet accessed in current transaction; looking up in snapshot
  auto const *snapshot_item = snapshot_cache_.try_get_snapshot_item(addrkey);

  // snapshot_item might be nullptr; this is handled in ItemView
  frame_stack_.back().add_item_view(addrkey, ItemView(snapshot_item));
  return *frame_stack_.back().get_item_view(addrkey);
}

} // namespace hogd
