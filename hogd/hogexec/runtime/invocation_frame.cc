#include "hogd/hogexec/runtime/invocation_frame.h"

#include <cassert>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/item_view.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

InvocationFrame::InvocationFrame(Invocation invocation)
  : active_invocation_(std::move(invocation))
{}

void
InvocationFrame::commit_successful_child_frame(InvocationFrame &frame)
{

  child_return_buffer_ = frame.to_return_;
  logs_.insert(logs_.end(), std::make_move_iterator(frame.logs_.begin()),
               std::make_move_iterator(frame.logs_.end()));

  for (auto const &[addr, hash] : frame.new_contract_deployments_) {
    [[maybe_unused]] auto [_, inserted_new] =
        new_contract_deployments_.emplace(addr, hash);
    assert(inserted_new);
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
InvocationFrameStack::start_frame(Invocation const &invocation)
{
  frame_stack_.emplace_back(invocation);
}

void
InvocationFrameStack::finish_frame(InvokeResult const &result)
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
    assert(!frame_stack_.empty());

    frame_stack_.pop_back();

    if (!frame_stack_.empty()) {
      // Contents of the frame are entirely dropped. We just need to notify
      // the parent (if there is one) that it had a failed frame.
      frame_stack_.back().commit_failed_child_frame();
    }
  }
  else {
    // commit child invocation frame successfully
    assert(result.is_frame_success());

    size_t stack_size = frame_stack_.size();
    if (stack_size >= 2) {
      // Commit the frame to it's parent
      frame_stack_[stack_size - 2].commit_successful_child_frame(
          frame_stack_.back());
    }
    else if (stack_size == 1) {
      // last frame -- push frame contents to external caller
      // (SystemCallHandler), via the supplied lambda
      last_frame_action_(frame_stack_.back());
    }

    frame_stack_.pop_back();
  }
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

ItemView *
InvocationFrame::get_item_view(AddressAndKey const &addrkey)
{
  auto it = modified_items_.find(addrkey);
  if (it != modified_items_.end()) {
    return &(it->second);
  }
  return nullptr;
}

ItemView *
InvocationFrame::add_item_view(AddressAndKey const &addrkey, ItemView new_view)
{
  assert(modified_items_.contains(addrkey));
  auto [iter, _] = modified_items_.emplace(addrkey, std::move(new_view));
  return &iter->second;
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
      // refresh the parent frame view in the top frame
      return *active_frame.add_item_view(addrkey, *local_view);
    }
  }

  // item view not yet accessed in current transaction; looking up in snapshot
  auto const* snapshot_item = snapshot_cache_.try_get_snapshot_item(addrkey);

  // snapshot_item might be nullptr; this is handled in ItemView
  return *frame_stack_.back().add_item_view(addrkey, ItemView(snapshot_item));
}

} // namespace hogd
