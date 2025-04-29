#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <cassert>
#include <memory>
#include <optional>
#include <utility>

#include "contract.hh"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

StorageSnapshotCache::StorageSnapshotCache(BlockID block_id,
                                           DatabaseProxy &db_proxy)
  : block_id_(block_id), db_proxy_(db_proxy)
{}

std::optional<SHA256Hash>
StorageSnapshotCache::try_get_deployed_contract_hash(
    ContractAddress const &addr)
{
  {
    auto [obj, ul] = deployment_cache_.find(addr);
    if (obj) {
      return *obj;
    }
  }

  auto db_res = db_proxy_.get_deployment(block_id_, addr)();

  // caches both positive and negative lookups
  deployment_cache_.insert(addr, db_res);
  return db_res;
}

std::shared_ptr<const Contract>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{
  {
    auto [obj, ul] = contract_cache_.find(hash);
    if (obj) {
      return *obj;
    }
  }

  auto db_res = db_proxy_.get_contract(block_id_, hash)();

  // TODO(geoff): If we decide to NOT reset the cache after every block,
  // then the contract cache needs to only cache positive lookups
  contract_cache_.insert(hash, db_res);
  return db_res;
}

const StorageItem *
StorageSnapshotCache::try_get_snapshot_item(AddressAndKey const &addrkey)
{
  {
    auto [obj, ul] = snapshot_item_cache_.find(addrkey);
    if (obj) {
      return obj->get();
    }
  }

  auto db_res = db_proxy_.get_snapshot_item(block_id_, addrkey)();
  auto const *out = db_res.get();
  snapshot_item_cache_.insert(addrkey, std::move(db_res));
  return out;
}

} // namespace hogd
