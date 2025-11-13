#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <cassert>
#include <format>
#include <memory>
#include <optional>
#include <variant>

#include "contract.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
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

StorageSnapshotCache::~StorageSnapshotCache() {
  cache_.clear();
}

SystemStatus<std::optional<SHA256Hash>>
StorageSnapshotCache::try_get_deployed_contract_hash(
    ContractAddress const &addr)
{
  DeploymentCacheKey key{addr};

  if (const auto* entry = cache_.get(key)) {
    CHECK(std::holds_alternative<std::optional<SHA256Hash>>(entry->data_));
    return std::get<std::optional<SHA256Hash>>(entry->data_);
  }

  auto db_res = db_proxy_.get_deployment(block_id_, addr);
  if (!db_res) {
    return db_res.error();
  }
  auto out = db_res.value();
  // caches both positive and negative lookups
  cache_.insert_or_get(key, out);
  return out;
}

SystemStatus<std::shared_ptr<const Contract>>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{
  ContractCacheKey key{hash};

  if (const auto* entry = cache_.get(key)) {
    CHECK(std::holds_alternative<std::shared_ptr<const Contract>>(entry->data_));
    return std::get<std::shared_ptr<const Contract>>(entry->data_);
  }

  auto db_res = db_proxy_.get_contract(block_id_, hash);
  if (!db_res) {
    return db_res.error();
  }
  auto out = db_res.value();

  if constexpr (is_debug_build()) {
    if (out) {
      auto received_hash = sha256(*out);
      if (received_hash != hash) {
        panic("Received incorrect contract hash from db: {} != {}",
              received_hash, hash);
      }
    }
  }

  // TODO(geoff): If we decide to NOT reset the cache after every block,
  // then the contract cache needs to only cache positive lookups
  cache_.insert_or_get(key, out);
  return out;
}

SystemStatus<SnapshotStorageItem *>
StorageSnapshotCache::try_get_snapshot_item(AddressAndKey const &addrkey)
{
  if (auto* entry = cache_.get(addrkey)) {
    CHECK(std::holds_alternative<SnapshotStorageItem>(entry->data_));
    return &std::get<SnapshotStorageItem>(entry->data_);
  }

  auto db_res = db_proxy_.get_snapshot_item(block_id_, addrkey);
  if (!db_res) {
    return db_res.error();
  }

  const static StorageItem deleted_item = StorageItem(StorageItemType::kItemTypeDeleted);
  const auto* item = &deleted_item;
  if (db_res.value().has_value()) {
    item = &*db_res.value();
  }

  auto* entry = cache_.insert_or_get(addrkey, *item, addrkey, block_id_, db_proxy_);
  CHECK_PTR(entry);
  return &std::get<SnapshotStorageItem>(entry->data_);
}

} // namespace hogd
