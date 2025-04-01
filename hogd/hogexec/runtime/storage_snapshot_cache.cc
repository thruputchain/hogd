#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <cassert>
#include <memory>
#include <optional>
#include <utility>

#include "contract.hh"
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

  auto db_res = db_proxy_.get_deployment(block_id_, addr)();

  // caches both positive and negative lookups
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
  auto db_res = db_proxy_.get_contract(block_id_, hash)();

  // TODO(geoff): If we decide to NOT reset the cache after every block,
  // then the contract cache needs to only cache positive lookups
  contract_cache_->emplace(hash, db_res);
  return db_res;
}

const StorageItem *
StorageSnapshotCache::try_get_snapshot_item(AddressAndKey const &addrkey)
{
  {
    auto cache = snapshot_item_cache_.take();
    auto it = snapshot_item_cache_->find(addrkey);
    if (it != cache->end()) {
      return it->second.get();
    }
  }

  auto db_res = db_proxy_.get_snapshot_item(block_id_, addrkey)();
  auto const *out = db_res.get();
  snapshot_item_cache_->emplace(addrkey, std::move(db_res));
  return out;
}

} // namespace hogd
