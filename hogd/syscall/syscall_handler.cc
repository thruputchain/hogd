#include "hogd/syscall/syscall_handler.h"

#include "hogd/base/types.h"
#include "hogd/syscall/gas_schedule.h"
#include "hogd/syscall/key_status.h"
#include "hogd/syscall/metered_result.h"
#include "hogd/syscall/proxy_applicator.h"
#include "hogd/syscall/syscall_status.h"

#include <cstring>

namespace hogd {

MeteredResult<uint64_t>
SyscallHandler::get_block_number() const
{
  return {kSyscallGetBlockNumberCost, 0};
}

MeteredResult<Hash>
SyscallHandler::get_prev_block_hash() const
{
  return {kSyscallGetPrevBlockHashCost, Hash{}};
}

SyscallStatus<AddressAndKey>
SyscallHandler::get_addrkey(StorageKey const &key)
{
  Address current_address =
      Address(); // Change, pending invocationstack impl which is pending x23c
  AddressAndKey out;

  static_assert(sizeof(Address) + sizeof(StorageKey) == sizeof(AddressAndKey),
                "type size error");
  std::memcpy(out.data(), current_address.data(), current_address.size());
  std::memcpy(out.data() + current_address.size(), key.data(), key.size());

  return out;
}

MeteredResult<ProxyApplicator *>
SyscallHandler::get_applicator(StorageKey const &key)
{
  auto addrkey = get_addrkey(key);
  if (!addrkey) {
    return {0, addrkey.error()};
  }

  auto it = storage_proxies_.find(*addrkey);
  if (it != storage_proxies_.end()) {
    return {kSyscallProxyLookupFoundCost, it->second.get()};
  }

  throw std::runtime_error("unimplemented");
}

MeteredResult<KeyStatus>
SyscallHandler::has_key(StorageKey const &key)
{
  return get_applicator(key) >> [](auto *app) -> MeteredResult<KeyStatus> {
    return app->has_key();
  };
}

} // namespace hogd
