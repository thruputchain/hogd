#include "hogd/hogexec/runtime/call_frame.h"

#include <cassert>
#include <unordered_map>

namespace hogd
{

void 
CallFrame::commit_successful_child_call(CallFrame&& frame)
{
	// overwrite any local storage proxies
	for (auto& [addr, proxy] : frame.storage_proxies_)
	{
		storage_proxies_[addr] = std::move(proxy);
	}
	visited_runtimes_.insert(frame.visited_runtimes_.begin(), frame.visited_runtimes_.end());
}

void 
CallFrame::push_new_storage_proxy(AddressAndKey const& new_addrkey, StorageProxy const& proxy)
{
	assert(storage_proxies_.find(new_addrkey) == storage_proxies_.end());
	storage_proxies_.emplace(new_addrkey, std::make_unique<StorageProxy>(proxy));
}

StorageProxy* 
CallFrame::try_get_storage_proxy(AddressAndKey const& query)
{
	auto it = storage_proxies_.find(query);

	if (it == storage_proxies_.end()) {
		return nullptr;
	}
	return it -> second.get();
}

}
