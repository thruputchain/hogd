#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"

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
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/snapshot_storage_item.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
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

SystemStatus<const ContractCacheEntry *>
StorageSnapshotCache::get_contract_entry(SHA256Hash const &hash)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = contract_cache_.find_shared(hash);
    if (obj) {
      return obj->has_value() ? &**obj : nullptr;
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

  // Build the entry, eagerly translating rv32 contracts so the deployment
  // back-pointer exposes a ready-to-instantiate image with no extra lookup.
  std::optional<ContractCacheEntry> entry;
  if (db_res.value()) {
    Contract contract = std::move(db_res.value()).value();
    std::optional<Rv32Contract> translated;
    if (contract.contract_type() == ContractType::kRv32) {
      translated = Rv32Contract::create(contract.rv32_contract());
    }
    entry.emplace(ContractCacheEntry{.contract = std::move(contract),
                                     .translated = std::move(translated)});
  }

  // TODO(geoff): If we decide to NOT reset the cache after every block,
  // then the contract cache needs to only cache positive lookups
  auto [obj, lock] = contract_cache_.insert(hash, std::move(entry));
  if (obj != nullptr) {
    return obj->has_value() ? &**obj : nullptr;
  }
  CHECK(lock.owns_lock());
  // concurrent insert happened: fetch the result from the cache again
  auto *obj2 = contract_cache_.find(hash, lock);
  CHECK_PTR(obj2);
  return obj2->has_value() ? &**obj2 : nullptr;
}

SystemStatus<const Contract *>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{
  auto entry = get_contract_entry(hash);
  if (!entry) {
    return entry.error();
  }
  return entry.value() ? &entry.value()->contract : nullptr;
}

SystemStatus<const DeployedContract *>
StorageSnapshotCache::try_get_deployed_contract(ContractAddress const &addr)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = deployment_cache_.find_shared(addr);
    if (obj) {
      return obj->has_value() ? &**obj : nullptr;
    }
  }

  auto db_res = db_proxy_.get_deployment(block_id_, addr);
  if (!db_res) {
    return db_res.error();
  }

  std::optional<DeployedContract> deployed;
  if (db_res.value()) {
    DeployedContractHash meta = db_res.value().value();
    // Eagerly warm + translate the contract code; back-point at its entry.
    auto entry = get_contract_entry(meta.hash);
    if (!entry) {
      return entry.error();
    }
    deployed.emplace(meta, entry.value());
  }

  auto [obj, lock] = deployment_cache_.insert(addr, std::move(deployed));
  if (obj != nullptr) {
    return obj->has_value() ? &**obj : nullptr;
  }
  CHECK(lock.owns_lock());
  // concurrent insert happened: fetch the result from the cache again
  auto *obj2 = deployment_cache_.find(addr, lock);
  CHECK_PTR(obj2);
  return obj2->has_value() ? &**obj2 : nullptr;
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

  auto entry =
      SnapshotStorageItem::make_variant(std::move(db_item), addrkey.key);
  if (!entry) {
    return entry.error();
  }

  auto [out, ul] =
      snapshot_item_cache_.insert(addrkey, std::move(entry).value(), addrkey,
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
