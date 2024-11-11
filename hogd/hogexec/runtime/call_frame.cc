#include "hogd/hogexec/runtime/call_frame.h"

#include <cassert>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/auth_context_stack.h"
#include "hogd/hogexec/runtime/auth_manager.h"
#include "hogd/hogexec/runtime/item_cache.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

CallFrame::CallFrame(Invocation invocation, AuthManager const &auth_manager,
                     AuthContextStack const &auth_context_stack, uint64_t flags,
                     ExtraFrameContext extra_context)
  : active_invocation_(invocation), auth_manager_(auth_manager),
    auth_context_stack_(auth_context_stack), flags_(flags),
    extra_context_(extra_context)
{
  visited_runtimes_.insert(active_invocation_.invoked_address);
}

void
CallFrame::commit_successful_child_call(CallFrame &frame)
{
  // overwrite any local item cache elements
  item_cache_map_.commit_child_map(frame.get_item_cache_map());

  visited_runtimes_.insert(frame.visited_runtimes_.begin(),
                           frame.visited_runtimes_.end());

  child_return_buffer_ = frame.to_return_;
  logs_.insert(logs_.end(), std::make_move_iterator(frame.logs_.begin()),
               std::make_move_iterator(frame.logs_.end()));

  auth_manager_ = std::move(frame.auth_manager_);
}

void
CallFrame::commit_failed_child_call()
{
  child_return_buffer_ = std::nullopt;
}

void
CallFrame::push_new_item_cache(AddressAndKey const &new_addrkey,
                               ItemCache const &proxy)
{
  assert(item_cache_map_.local_map_.find(new_addrkey) ==
         item_cache_map_.local_map_.end());
  item_cache_map_.local_map_.emplace(new_addrkey, proxy);
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

ItemCache *
CallFrame::try_get_item_cache(AddressAndKey const &query)
{
  auto it = item_cache_map_.local_map_.find(query);

  if (it == item_cache_map_.local_map_.end()) {
    return nullptr;
  }
  return &(it->second);
}

void
CallFrame::set_call_return(ReturnBuffer &&return_buffer)
{
  to_return_ = std::move(return_buffer);
}

} // namespace hogd
