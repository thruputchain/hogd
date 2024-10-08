#include "hogd/hogexec/runtime/storage_snapshot_cache.h"

#include <cassert>
#include <memory>
#include <optional>
#include <span>

#include "hogd/crypto/crypto.h"
#include "x23/serialize.h"

#include "contract.hh"
#include "storage_object.hh"
#include "types.hh"

namespace hogd {

std::optional<StorageObject>
StorageSnapshotCache::try_get_object(AddressAndKey const &addrkey)
{
  {
    auto cache = storage_cache_.take();

    auto it = cache->find(addrkey);

    if (it != cache->end()) {
      return it->second;
    }
  }

  // TODO(geoff) do request to storage
  std::optional<StorageObject> rpc_result;

  storage_cache_->emplace(addrkey, rpc_result);

  return rpc_result;
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

  // TODO(geoff) do request to storage
  std::optional<SHA256Hash> hash_from_db = std::nullopt;

  deployment_cache_->emplace(addr, hash_from_db);

  return hash_from_db;
}

std::shared_ptr<Contract>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{
  {
    auto cache = contract_cache_.take();

    auto it = cache->find(hash);

    if (it != cache->end()) {
      return it->second;
    }
  }

  // TODO(geoff) do request to storage
  std::shared_ptr<Contract> contract_from_db;

  contract_cache_->emplace(hash, contract_from_db);

  return contract_from_db;
}

namespace test {

SHA256Hash
LocalMockStorageSnapshot::register_contract(std::shared_ptr<Contract> &contract)
{
  assert(!!contract);

  auto buf = x23::serialize(*contract);

  SHA256Hash hash = sha256(std::as_bytes(std::span(buf)));

  contract_cache_->emplace(hash, contract);
  return hash;
}

void
LocalMockStorageSnapshot::deploy_contract(ContractAddress const &addr,
                                          SHA256Hash const &hash)
{
  deployment_cache_->emplace(addr, hash);
}

} // namespace test

} // namespace hogd
