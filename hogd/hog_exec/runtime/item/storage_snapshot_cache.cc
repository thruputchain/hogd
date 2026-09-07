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
#include "hogd/hog_exec/runtime/item/frame_item_key.h"
#include "hogd/hog_exec/runtime/item/snapshot_storage_item.h"
#include "hogd/hog_exec/runtime/prepared_contract.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

StorageSnapshotCache::StorageSnapshotCache(
    BlockID block_id, BlockExecutionContext const &block_execution,
    DatabaseProxy &db_proxy)
  : block_id_(block_id), block_execution_context_(block_execution),
    db_proxy_(db_proxy)
{}

StorageSnapshotCache::~StorageSnapshotCache()
{
  release_db_tip();
}

x23::MaybeAsync<SystemStatus<const ContractCacheEntry *>>
StorageSnapshotCache::try_get_contract_entry(SHA256Hash const &hash)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = contract_cache_.find_shared(hash);
    if (obj) {
      return SystemStatus<const ContractCacheEntry *>{
          obj->has_value() ? &**obj : nullptr};
    }
  }
  return load_contract_entry(hash);
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
    PreparedContract prepared;
    if (contract.contract_type() == ContractType::kRv32) {
      prepared.rv32 = Rv32Contract::create(contract.rv32_contract());
    }
    entry.emplace(ContractCacheEntry{.contract = std::move(contract),
                                     .prepared = std::move(prepared)});
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

x23::MaybeAsync<SystemStatus<const DeployedContract *>>
StorageSnapshotCache::try_get_deployed_contract(ContractAddress const &addr)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = deployment_cache_.find_shared(addr);
    if (obj) {
      return SystemStatus<const DeployedContract *>{obj->has_value() ? &**obj
                                                                     : nullptr};
    }
  }
  return load_deployed_contract(addr);
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
    auto entry = co_await try_get_contract_entry(meta.hash);
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

x23::MaybeAsync<SystemStatus<const SnapshotStorageItem *>>
StorageSnapshotCache::try_get_snapshot_item(FrameItemKey const &key)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = snapshot_item_cache_.find_shared(key);
    if (obj) {
      return SystemStatus<const SnapshotStorageItem *>{obj};
    }
  }
  return load_snapshot_item(key);
}

x23::Async<SystemStatus<const SnapshotStorageItem *>>
StorageSnapshotCache::load_snapshot_item(FrameItemKey key)
{
  // The one place the XDR form is needed: the database reads by it, and the
  // item keeps it for the retrocausal-queue reads it may make later.
  const AddressAndKey addrkey = key.to_address_and_key();
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

  auto [out, ul] = snapshot_item_cache_.insert(key, std::move(entry).value(),
                                               addrkey, block_id_, db_proxy_);
  if (out) {
    // insertion happened; return the result
    co_return out;
  }
  CHECK(ul.owns_lock());

  auto *obj = snapshot_item_cache_.find(key, ul);
  // otherwise, lookup the item in the cache again
  CHECK_PTR(obj);
  co_return obj;
}

x23::Async<SystemStatus<void>>
StorageSnapshotCache::load_db_tip()
{
  auto db_res = co_await db_proxy_.get_db_tip(block_id_);
  if (!db_res) {
    co_return db_res.error();
  }

  // The height the block was opened with must agree with the tip just read,
  // or this node is executing a different block than it is building. Checked
  // here, the block's one fallible tip read, so every later height read stays
  // a plain load.
  const BlockHeight derived = db_res->value.proposal_header.block_height + 1;
  if (block_execution_context_.block_height != derived) [[unlikely]] {
    error("Block height mismatch: opened with {}, db tip implies {}",
          block_execution_context_.block_height, derived);
    co_return std::unexpected(SystemError{SystemErrorCode::Unrecoverable});
  }

  set_db_tip(std::move(db_res.value()));
  co_return SystemStatus<void>{};
}

void
StorageSnapshotCache::set_db_tip(DbTip tip)
{
  CHECK(db_tip_ == nullptr);
  CHECK_EQ(block_execution_context_.block_height,
           tip.value.proposal_header.block_height + 1);
  db_tip_ = std::make_unique<DbTip>(std::move(tip));
  network_config_params_ =
      std::make_unique<NetworkConfigParams>(db_tip_->network_config.v0());
}

void
StorageSnapshotCache::release_db_tip()
{
  db_tip_.reset();
  network_config_params_.reset();
}

} // namespace hogd
