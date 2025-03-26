#include "hogd/hogexec/runtime/invocation_frame.h"

#include <cassert>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"
#include "hogd/hogexec/runtime/status.h"

namespace hogd {

InvocationFrame::InvocationFrame(Invocation invocation)
  : active_invocation_(std::move(invocation))
{
}

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

} // namespace hogd
