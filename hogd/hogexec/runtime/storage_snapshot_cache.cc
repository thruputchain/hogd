#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <cassert>
#include <memory>
#include <optional>

#include "contract.hh"
#include "storage_interface.hh"
#include "storage_object.hh"
#include "types.hh"

namespace hogd {

std::optional<StrippedStorageObject>
StorageSnapshotCache::try_get_object(AddressAndKey const &addrkey)
{
  {
    auto cache = storage_cache_.take();

    auto it = cache->find(addrkey);

    if (it != cache->end()) {
      return it->second;
    }
  }

  auto client_res = client_.get_object(current_block_hash_, addrkey)();

  if (client_res.error() == StorageRPCError::kNone) {
    storage_cache_->emplace(addrkey, client_res.object());
    return client_res.object();
  }
  return std::nullopt;
}

std::optional<SHA256Hash>
StorageSnapshotCache::try_get_deployed_contract_hash(
    ContractAddress const &addr)
{
  {
    auto cache = deployment_cache_.take();

    auto it = cache->find(addr);

    if (it != cache->end()) {
      return it->second;
    }
  }

  auto client_res = client_.get_deployment(addr)();

  if (client_res.error() == StorageRPCError::kNone) {
    deployment_cache_->emplace(addr, client_res.deployed_hash());
    return client_res.deployed_hash();
  }

  return std::nullopt;
}

std::shared_ptr<const Contract>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{
  {
    auto cache = contract_cache_.take();

    auto it = cache->find(hash);

    if (it != cache->end()) {
      return it->second;
    }
  }

  auto client_res = client_.get_contract(hash)();

  if (client_res.error() == StorageRPCError::kNone) {
    std::shared_ptr<const Contract> contract_from_db =
        std::make_shared<const Contract>(client_res.contract());
    contract_cache_->emplace(hash, contract_from_db);
    return contract_from_db;
  }

  return nullptr;
}

} // namespace hogd
