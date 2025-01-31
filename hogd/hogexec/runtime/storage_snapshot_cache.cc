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
  auto key = DeploymentMapEntry::get_key(addr);
  if (auto res = deployment_cache_.modify_element(key, [](auto const &v) {
        return v.hash_;
      })) {
    return *res;
  }

  // cache positive and negative lookups
  auto db_res = db_proxy_.get_deployment(block_id_, addr);
  deployment_cache_.emplace(key, addr, db_res);
  return db_res;
}

std::shared_ptr<const Contract>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{

  auto key = ContractMapEntry::get_key(hash);
  if (auto res = contract_cache_.modify_element(key, [](auto const &v) {
        return v.contract_;
      })) {
    return *res;
  }

  // cache positive and negative lookups
  auto db_res = db_proxy_.get_contract(block_id_, hash);
  contract_cache_.emplace(key, hash, db_res);
  return db_res;
}

const StorageItem *
StorageSnapshotCache::try_get_snapshot_item(AddressAndKey const &addrkey)
{
  auto key = StorageItemMapEntry::get_key(addrkey);
  if (auto res = snapshot_item_cache_.modify_element(key, [](auto const &v) {
        return v.item_.get();
      })) {
    return *res;
  }

  // cache positive and negative lookups
  auto db_res = db_proxy_.get_item(block_id_, addrkey);
  std::unique_ptr<StorageItem> item =
      db_res ? std::make_unique<StorageItem>(*db_res) : nullptr;
  auto const *out = item.get();

  snapshot_item_cache_.emplace(key, addrkey, std::move(item));
  return out;
}
} // namespace hogd
