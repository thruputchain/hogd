#include "hogd/hogexec/runtime/call_frame.h"

#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/storage_proxy.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

CallFrame::CallFrame(Invocation invocation, WasmiRuntime &runtime)
  : active_invocation_(std::move(invocation)), active_runtime_(runtime)
{}

void
CallFrame::commit_successful_child_call(CallFrame &&frame)
{
  // overwrite any local storage proxies
  for (auto &[addr, proxy] : frame.storage_proxies_) {
    storage_proxies_[addr] = std::move(proxy);
  }
  visited_runtimes_.insert(frame.visited_runtimes_.begin(),
                           frame.visited_runtimes_.end());

  child_return_buffer_ = frame.to_return_;
  logs_.insert(logs_.end(), std::make_move_iterator(frame.logs_.begin()),
               std::make_move_iterator(frame.logs_.end()));
}

void
CallFrame::commit_failed_child_call()
{
  child_return_buffer_ = std::nullopt;
}

void
CallFrame::push_new_storage_proxy(AddressAndKey const &new_addrkey,
                                  StorageProxy const &proxy)
{
  assert(storage_proxies_.find(new_addrkey) == storage_proxies_.end());
  storage_proxies_.emplace(new_addrkey, std::make_unique<StorageProxy>(proxy));
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

StorageProxy *
CallFrame::try_get_storage_proxy(AddressAndKey const &query)
{
  auto it = storage_proxies_.find(query);

  if (it == storage_proxies_.end()) {
    return nullptr;
  }
  return it->second.get();
}

} // namespace hogd
