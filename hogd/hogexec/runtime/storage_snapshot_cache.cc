#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <cassert>
#include <format>
#include <memory>
#include <optional>
#include <utility>

#include "contract.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/snapshot_storage_item.h"
#include "hogd/hogexec/runtime/status.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

StorageSnapshotCache::StorageSnapshotCache(BlockID block_id,
                                           DatabaseProxy &db_proxy)
  : block_id_(block_id), db_proxy_(db_proxy)
{}

StorageSnapshotCache::~StorageSnapshotCache() = default;

SystemStatus<std::optional<SHA256Hash>>
StorageSnapshotCache::try_get_deployed_contract_hash(
    ContractAddress const &addr)
{
  {
    auto [obj, ul] = deployment_cache_.find(addr);
    if (obj) {
      return *obj;
    }
  }

  auto db_res = db_proxy_.get_deployment(block_id_, addr);
  if (!db_res) {
    return db_res.error();
  }
  auto out = db_res.value();
  // caches both positive and negative lookups
  deployment_cache_.insert(addr, std::move(db_res.value()));
  return out;
}

SystemStatus<std::shared_ptr<const Contract>>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{
  {
    auto [obj, ul] = contract_cache_.find(hash);
    if (obj) {
      return *obj;
    }
  }

  auto db_res = db_proxy_.get_contract(block_id_, hash);
  if (!db_res) {
    return db_res.error();
  }
  auto out = db_res.value();

  if constexpr (is_debug_build()) {
    auto received_hash = sha256(*out);
    if (received_hash != hash) {
      panic("Received incorrect contract hash from db: {} != {}", received_hash,
            hash);
    }
  }

  // TODO(geoff): If we decide to NOT reset the cache after every block,
  // then the contract cache needs to only cache positive lookups
  contract_cache_.insert(hash, std::move(db_res.value()));
  return out;
}

namespace {
std::unique_ptr<SnapshotStorageItem>
make_snapshot_item(std::optional<StorageItem> db_item,
                   AddressAndKey const &addrkey, BlockID block_id,
                   DatabaseProxy &db_proxy)
{
  if (!db_item) {
    return std::make_unique<SnapshotStorageItem>(
        StorageItem(StorageItemType::kItemTypeDeleted), addrkey, block_id,
        db_proxy);
  }
  return std::make_unique<SnapshotStorageItem>(*db_item, addrkey, block_id,
                                               db_proxy);
}
} // namespace

SystemStatus<SnapshotStorageItem *>
StorageSnapshotCache::try_get_snapshot_item(AddressAndKey const &addrkey)
{
  {
    auto [obj, ul] = snapshot_item_cache_.find(addrkey);
    if (obj) {
      assert(obj->get());
      return obj->get();
    }
  }

  auto db_res = db_proxy_.get_snapshot_item(block_id_, addrkey);
  if (!db_res) {
    return db_res.error();
  }

  // cache both positive and negative lookups
  auto ptr = make_snapshot_item(db_res.value(), addrkey, block_id_, db_proxy_);
  {
    auto [out, ul] = snapshot_item_cache_.insert(addrkey, std::move(ptr));
    if (out) {
      // insertion happened; return the result
      return out->get();
    }
  }
  // otherwise, lookup the item in the cache again
  auto [obj, ul] = snapshot_item_cache_.find(addrkey);
  assert(obj);
  assert(obj->get());
  return obj->get();
}

} // namespace hogd
