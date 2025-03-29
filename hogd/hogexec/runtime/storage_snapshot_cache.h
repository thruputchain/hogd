#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

#include "contract.hh"
#include "hogd/base/hashmap.h"
#include "types.hh"

namespace hogd {

namespace detail {

struct CachedContract {
  SHA256Hash contract_hash_;
  std::shared_ptr<const Contract> contract_;

  std::span<const std::byte> get_key() const
  {
    return std::as_bytes(std::span(contract_hash_));
  }
};

struct CachedDeployment {
  ContractAddress address_;
  SHA256Hash deployed_hash_;

  std::span<const std::byte> get_key() const
  {
    return std::as_bytes(std::span(address_));
  }
};

} // namespace detail

// Root cache for objects read from the ledger.
// Can be shared between concurrent transactions,
// but must be reset after each block.
//
// TODO(geoff): Not everything needs to be reset after every block.
// The key example is the cache of contracts.
// We might, therefore, move the contracts into some kind of LRU cache.
// Or in general change the caching policy of any type of object.
//
// TODO(geoff): These could be combined into one object, but then we need
// some mechanism to ensure domain separation between key types.
// This is possible with some e.g. higher level mechanism (if we can, for
// example, ensure that all ContractAddresses are different from deployed
// contract hashes), but that seems bugprone and difficult to maintain.
class StorageSnapshotCache {
public:
  std::shared_ptr<const Contract>
  try_get_contract_by_hash(SHA256Hash const &hash);
  std::optional<SHA256Hash>
  try_get_deployed_contract_hash(ContractAddress const &addr);

private:
  HashMap<detail::CachedContract> contract_cache_;
  HashMap<detail::CachedDeployment> deployment_cache_;
};

} // namespace hogd
