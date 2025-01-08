#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <cassert>
#include <memory>
#include <optional>

#include "contract.hh"
#include "storage_object.hh"
#include "types.hh"

namespace hogd {

std::optional<SHA256Hash>
StorageSnapshotCache::try_get_deployed_contract_hash(
    ContractAddress const &addr)
{
  {
    auto cache = deployment_cache_.take();
    auto it = cache->find(addr);
    if (it != cache->end()) {
      return it->second;
    }
  }

  // TODO(geoff): query from storage (unimplemented here)

  return std::nullopt;
}

std::shared_ptr<const Contract>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{
  {
    auto cache = contract_cache_.take();
    auto it = cache->find(hash);
    if (it != cache->end()) {
      return it->second;
    }
  }

  // TODO(geoff): query from storage (unimplemented here)

  return nullptr;
}

const StorageObject *
StorageSnapshotCache::try_get_snapshot_object(AddressAndKey const &addrkey)
{
  {
    auto cache = snapshot_object_cache_.take();
    auto it = cache->find(addrkey);
    if (it != cache->end()) {
      return it->second.get();
    }
  }

  // TODO(geoff): query from storage (unimplemented here)

  return nullptr;
}

} // namespace hogd
