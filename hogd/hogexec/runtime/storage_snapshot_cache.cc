#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <cassert>
#include <memory>
#include <optional>
#include <span>

#include "contract.hh"
#include "types.hh"

namespace hogd {

std::optional<SHA256Hash>
StorageSnapshotCache::try_get_deployed_contract_hash(
    ContractAddress const &addr)
{
  auto key = std::as_bytes(std::span(addr));
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
  auto key = std::as_bytes(std::span(hash));
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

} // namespace hogd
