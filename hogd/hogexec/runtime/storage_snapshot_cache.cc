#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <cassert>
#include <memory>
#include <optional>

#include "contract.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

std::optional<SHA256Hash>
StorageSnapshotCache::try_get_deployed_contract_hash(
    ContractAddress const &addr)
{
  auto key = detail::CachedDeployment::get_key(addr);
  auto cached_obj =
      deployment_cache_.modify_element(key, [&](auto const &v) -> SHA256Hash {
        assert(v.address_ == addr);
        return v.deployed_hash_;
      });

  if (cached_obj.has_value()) {
    return cached_obj;
  }
  // TODO(geoff): query from storage (unimplemented here)

  return std::nullopt;
}

std::shared_ptr<const Contract>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{
  auto key = detail::CachedContract::get_key(hash);
  auto cached_obj = contract_cache_.modify_element(
      key, [&](auto const &v) -> std::shared_ptr<const Contract> {
        assert(v.contract_hash_ == hash);
        return v.contract_;
      });
  if (cached_obj.has_value()) {
    return *cached_obj;
  }

  // TODO(geoff): query from storage (unimplemented here)

  return nullptr;
}

std::optional<StorageItem>
StorageSnapshotCache::try_get_snapshot_item(AddressAndKey const &addrkey)
{
  auto key = detail::CachedStorageItem::get_key(addrkey);

  auto cached_obj = item_cache_.modify_element(
      key, [&](auto const &v) -> std::optional<StorageItem> {
        assert(v.addrkey_ == addrkey);
        return v.item_;
      });

  if (cached_obj.has_value()) {
    // The item cache caches positive and negative lookups;
    // cached_obj is a std::optional<std::optional<StorageItem>>,
    // signaling first whether the item is in the cache
    // and second, if present in the cache, whether the item actually
    // exists in the snapshot.
    return *cached_obj;
  }

  // TODO(geoff): query from storage (unimplemented here)

  return std::nullopt;
}

} // namespace hogd
