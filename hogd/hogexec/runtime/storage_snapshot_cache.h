#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/base/xdr_util.h"
#include "storage_item.hh"
#include "types.hh"
#include "x23/mvar.h"

namespace hogd {

// Root cache for objects read from the ledger.
// Can be shared between concurrent transactions,
// but must be reset after each block.
//
// This is not designed as a high performance implementation.
// If this is a performance bottleneck, it can be swapped out
// for something with this same interface.
class StorageSnapshotCache {
public:
  std::shared_ptr<const Contract>
  try_get_contract_by_hash(SHA256Hash const &hash);
  std::optional<SHA256Hash>
  try_get_deployed_contract_hash(ContractAddress const &addr);
  const StorageItem *try_get_snapshot_item(AddressAndKey const &addrkey);

private:
  // Note: the contract_cache_ does not need to be reset (although it is not
  // incorrect to reset it) after every block.
  // Future PRs will have a storage object cache, which will need to be reset.
  x23::MVar<std::map<SHA256Hash, std::shared_ptr<const Contract>>>
      contract_cache_;
  // Neither does the deployment cache
  x23::MVar<std::map<ContractAddress, std::optional<SHA256Hash>>>
      deployment_cache_;
  // snapshot cache needs to be reset after each block
  x23::MVar<std::unordered_map<AddressAndKey, std::unique_ptr<StorageItem>>>
      snapshot_item_cache_;
};

} // namespace hogd
