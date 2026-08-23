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
#include "x23/async.h"

namespace hogd {

StorageSnapshotCache::StorageSnapshotCache(BlockID block_id,
                                           DatabaseProxy &db_proxy)
  : block_id_(block_id), db_proxy_(db_proxy)
{}

StorageSnapshotCache::~StorageSnapshotCache()
{
  release_db_tip();
}

x23::Async<SystemStatus<const ContractCacheEntry *>>
StorageSnapshotCache::get_contract_entry(SHA256Hash hash)
{
  {
    // Use shared lock for read-only cache lookup.  The scope ends before the
    // await below, so no cache lock is ever held across a suspension.
    auto [obj, sl] = contract_cache_.find_shared(hash);
    if (obj) {
      co_return SystemStatus<const ContractCacheEntry *>{
          obj->has_value() ? &**obj : nullptr};
    }
  }
  co_return co_await load_contract_entry(hash);
}

x23::Async<SystemStatus<const ContractCacheEntry *>>
StorageSnapshotCache::load_contract_entry(SHA256Hash hash)
{
  auto db_res = co_await db_proxy_.get_contract(block_id_, hash);
  if (!db_res) {
    co_return db_res.error();
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
    co_return obj->has_value() ? &**obj : nullptr;
  }
  CHECK(lock.owns_lock());
  // concurrent insert happened: fetch the result from the cache again
  auto *obj2 = contract_cache_.find(hash, lock);
  CHECK_PTR(obj2);
  co_return obj2->has_value() ? &**obj2 : nullptr;
}

x23::Async<SystemStatus<const Contract *>>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash hash)
{
  {
    auto [obj, sl] = contract_cache_.find_shared(hash);
    if (obj) {
      co_return SystemStatus<const Contract *>{
          obj->has_value() ? &(*obj)->contract : nullptr};
    }
  }
  co_return co_await load_contract_by_hash(hash);
}

x23::Async<SystemStatus<const Contract *>>
StorageSnapshotCache::load_contract_by_hash(SHA256Hash hash)
{
  auto entry = co_await load_contract_entry(hash);
  if (!entry) {
    co_return entry.error();
  }
  co_return entry.value() ? &entry.value()->contract : nullptr;
}

x23::Async<SystemStatus<const DeployedContract *>>
StorageSnapshotCache::try_get_deployed_contract(ContractAddress addr)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = deployment_cache_.find_shared(addr);
    if (obj) {
      co_return SystemStatus<const DeployedContract *>{
          obj->has_value() ? &**obj : nullptr};
    }
  }
  co_return co_await load_deployed_contract(addr);
}

x23::Async<SystemStatus<const DeployedContract *>>
StorageSnapshotCache::load_deployed_contract(ContractAddress addr)
{
  auto db_res = co_await db_proxy_.get_deployment(block_id_, addr);
  if (!db_res) {
    co_return db_res.error();
  }

  std::optional<DeployedContract> deployed;
  if (db_res.value()) {
    DeployedContractHash meta = db_res.value().value();
    // Eagerly warm + translate the contract code; back-point at its entry.
    auto entry = co_await get_contract_entry(meta.hash);
    if (!entry) {
      co_return entry.error();
    }
    deployed.emplace(meta, entry.value());
  }

  auto [obj, lock] = deployment_cache_.insert(addr, std::move(deployed));
  if (obj != nullptr) {
    co_return obj->has_value() ? &**obj : nullptr;
  }
  CHECK(lock.owns_lock());
  // concurrent insert happened: fetch the result from the cache again
  auto *obj2 = deployment_cache_.find(addr, lock);
  CHECK_PTR(obj2);
  co_return obj2->has_value() ? &**obj2 : nullptr;
}

x23::Async<SystemStatus<const SnapshotStorageItem *>>
StorageSnapshotCache::try_get_snapshot_item(AddressAndKey addrkey)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = snapshot_item_cache_.find_shared(addrkey);
    if (obj) {
      co_return SystemStatus<const SnapshotStorageItem *>{obj};
    }
  }
  co_return co_await load_snapshot_item(addrkey);
}

x23::Async<SystemStatus<const SnapshotStorageItem *>>
StorageSnapshotCache::load_snapshot_item(AddressAndKey addrkey)
{
  auto db_res = co_await db_proxy_.get_snapshot_item(block_id_, addrkey);
  if (!db_res) {
    co_return db_res.error();
  }

  // cache both positive and negative lookups
  WireStorageItem db_item =
      db_res.value().has_value()
          ? db_res.value().value()
          : WireStorageItem(StorageItemType::kItemTypeDeleted);

  auto entry =
      SnapshotStorageItem::make_variant(std::move(db_item), addrkey.key);
  if (!entry) {
    co_return entry.error();
  }

  auto [out, ul] =
      snapshot_item_cache_.insert(addrkey, std::move(entry).value(), addrkey,
                                  block_id_, db_proxy_);
  if (out) {
    // insertion happened; return the result
    co_return out;
  }
  CHECK(ul.owns_lock());

  auto *obj = snapshot_item_cache_.find(addrkey, ul);
  // otherwise, lookup the item in the cache again
  CHECK_PTR(obj);
  co_return obj;
}

x23::Async<SystemStatus<const DbTip *>>
StorageSnapshotCache::try_get_db_tip()
{
  if (auto *tip = db_tip_.load(); tip != nullptr) {
    co_return SystemStatus<const DbTip *>{tip};
  }
  co_return co_await load_db_tip();
}

x23::Async<SystemStatus<const DbTip *>>
StorageSnapshotCache::load_db_tip()
{
  auto db_res = co_await db_proxy_.get_db_tip(block_id_);
  if (!db_res) {
    co_return db_res.error();
  }

  std::unique_ptr<DbTip> tip_unique = std::make_unique<DbTip>(db_res.value());
  DbTip *expected = nullptr;
  if (db_tip_.compare_exchange_strong(expected, tip_unique.get())) {
    co_return tip_unique.release();
  }
  CHECK_PTR(expected);
  co_return expected;
}

void
StorageSnapshotCache::release_db_tip()
{
  auto *tip = db_tip_.exchange(nullptr);
  delete tip;
}

} // namespace hogd
