#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <optional>

#include "types.hh"

namespace hogd {

std::optional<StorageObject> 
StorageSnapshotCache::try_get_object(AddressAndKey const &addrkey)
{
  {
    auto cache = storage_cache_.take();

    auto it = cache.find(addrkey);

    if (it != cache.end()) {
      return it->second;
    }
  }

  // TODO(geoff) do request to storage
  std::optional<StorageObject> rpc_result;

  cache->emplace(addrkey, rpc_result);

  return rpc_result;
}

std::shared_ptr<Contract>
StorageSnapshotCache::try_get_contract_by_addr(ContractAddress const& addr)
{
  {
    auto cache = contract_cache_.take();

    auto it = cache.find(addr);

    if (it != cache.end()) {
      return it->second;
    }
  }

  // TODO(geoff) do request to storage
  std::shared_ptr<Contract> contract_from_db;

  cache->emplace(addrkey, contract_from_db);

  return contract_from_db;
}



}