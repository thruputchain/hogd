#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"

#include <cassert>
#include <format>
#include <memory>
#include <optional>
#include <utility>

#include "contract.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/snapshot_storage_item.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

StorageSnapshotCache::StorageSnapshotCache(BlockID block_id,
                                           DatabaseProxy &db_proxy)
  : block_id_(block_id), db_proxy_(db_proxy)
{}

StorageSnapshotCache::~StorageSnapshotCache()
{
  release_db_tip();
}

SystemStatus<std::optional<DeployedContractHash>>
StorageSnapshotCache::try_get_deployed_contract_hash(
    ContractAddress const &addr)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = deployment_cache_.find_shared(addr);
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

SystemStatus<const Contract *>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = contract_cache_.find_shared(hash);
    if (obj) {
      if (obj->has_value()) {
        return &obj->value();
      }
      return nullptr;
    }
  }

  auto db_res = db_proxy_.get_contract(block_id_, hash);
  if (!db_res) {
    return db_res.error();
  }

  if constexpr (is_debug_build()) {
    if (db_res.value()) {
      auto received_hash = sha256(*db_res.value());
      if (received_hash != hash) {
        panic("Received incorrect contract hash from db: {} != {}",
              show_byte_array(received_hash), show_byte_array(hash));
      }
    }
  }

  // TODO(geoff): If we decide to NOT reset the cache after every block,
  // then the contract cache needs to only cache positive lookups
  auto [obj, lock] = contract_cache_.insert(hash, std::move(db_res.value()));
  if (obj != nullptr) {
    if (obj->has_value()) {
      return &obj->value();
    }
    return nullptr;
  }
  CHECK(lock.owns_lock());
  // concurrent insert happened: fetch the result from the cache again
  auto *obj2 = contract_cache_.find(hash, lock);
  CHECK_PTR(obj2);
  if (obj2->has_value()) {
    return &obj2->value();
  }
  return nullptr;
}

SystemStatus<const SnapshotStorageItem *>
StorageSnapshotCache::try_get_snapshot_item(AddressAndKey const &addrkey)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = snapshot_item_cache_.find_shared(addrkey);
    if (obj) {
      return obj;
    }
  }

  auto db_res = db_proxy_.get_snapshot_item(block_id_, addrkey);
  if (!db_res) {
    return db_res.error();
  }

  // cache both positive and negative lookups
  WireStorageItem db_item =
      db_res.value().has_value()
          ? db_res.value().value()
          : WireStorageItem(StorageItemType::kItemTypeDeleted);

  auto [out, ul] = snapshot_item_cache_.insert(addrkey, db_item, addrkey,
                                               block_id_, db_proxy_);
  if (out) {
    // insertion happened; return the result
    return out;
  }
  CHECK(ul.owns_lock());

  auto *obj = snapshot_item_cache_.find(addrkey, ul);
  // otherwise, lookup the item in the cache again
  CHECK_PTR(obj);
  return obj;
}

SystemStatus<const DbTip *>
StorageSnapshotCache::try_get_db_tip()
{
  auto *tip = db_tip_.load();
  if (tip != nullptr) {
    return tip;
  }

  auto db_res = db_proxy_.get_db_tip(block_id_);
  if (!db_res) {
    return db_res.error();
  }

  std::unique_ptr<DbTip> tip_unique = std::make_unique<DbTip>(db_res.value());
  if (db_tip_.compare_exchange_strong(tip, tip_unique.get())) {
    return tip_unique.release();
  }
  CHECK_PTR(tip);
  return tip;
}

void
StorageSnapshotCache::release_db_tip()
{
  auto *tip = db_tip_.exchange(nullptr);
  delete tip;
}

} // namespace hogd
