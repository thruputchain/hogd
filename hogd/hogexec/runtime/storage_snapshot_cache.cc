#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <cassert>
#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "storage_item.hh"
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

  // cache positive and negative lookups
  auto db_res = db_proxy_.get_deployment(block_id_, addr);
  deployment_cache_->emplace(addr, db_res);
  return db_res;
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

  // cache both positive and negative lookups
  auto db_res = db_proxy_.get_contract(block_id_, hash);
  contract_cache_->emplace(hash, db_res);
  return db_res;
}

const StorageItem *
StorageSnapshotCache::try_get_snapshot_item(AddressAndKey const &addrkey)
{
  auto cache = snapshot_item_cache_.take();
  auto it = cache->find(addrkey);
  if (it != cache->end()) {
    return it->second.get();
  }

  auto db_res = db_proxy_.get_item(block_id_, addrkey);
  if (db_res) {
    cache->emplace(addrkey, std::make_unique<StorageItem>(*db_res));
  }
  else {
    cache->emplace(addrkey, nullptr);
  }

  return cache->at(addrkey).get();
}

} // namespace hogd
