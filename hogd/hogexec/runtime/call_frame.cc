#include "hogd/hogexec/runtime/call_frame.h"

#include <cassert>
#include <memory>
#include <optional>
#include <utility>

#include "hogd/hogexec/runtime/storage_proxy.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "types.hh"
#include "transaction.hh"

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
}

void
CallFrame::commit_failed_child_call()
{
  child_return_buffer_ = std::nullopt;
}

void
CallFrame::push_new_storage_proxy(AddressAndKey const &new_addrkey,
                                  StorageProxy const& proxy)
{
  assert(storage_proxies_.find(new_addrkey) == storage_proxies_.end());
  storage_proxies_.emplace(new_addrkey, std::make_unique<StorageProxy>(proxy));
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
