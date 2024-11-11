#include "hogd/hogexec/runtime/call_frame.h"

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

namespace hogd {

CallFrame::CallFrame(Invocation invocation)
  : active_invocation_(std::move(invocation))
{
  visited_runtimes_.insert(active_invocation_.invoked_address);
}

void
CallFrame::commit_successful_child_call(CallFrame &frame)
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
}

void
CallFrame::commit_failed_child_call()
{
  child_return_buffer_ = std::nullopt;
}

void
CallFrame::log(std::span<const std::byte> message)
{
  std::span<const uint8_t> m = {reinterpret_cast<const uint8_t *>(
                                    message.data()),
                                message.size()};
  logs_.emplace_back(active_invocation_.invoked_address,
                     x23::Vec<uint8_t, kMaxLogMessageLen>(m.begin(), m.end()));
}

void
CallFrame::set_call_return(ReturnBuffer &&return_buffer)
{
  to_return_ = std::move(return_buffer);
}

} // namespace hogd
