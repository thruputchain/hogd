#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <map>

#include "storage_object.hh"
#include "types.hh"
#include "contract.hh"
#include "x23/mvar.h"

namespace hogd {

// Root cache for objects read from the ledger.
// Can be shared between concurrent transactions,
// but must be reset after each block.
class StorageSnapshotCache {
public:
  std::optional<StorageObject> try_get_object(AddressAndKey const &addrkey);

  std::shared_ptr<Contract>
  try_get_contract_by_hash(SHA256Hash const &hash);

  std::optional<SHA256Hash>
  try_get_deployed_contract_hash(ContractAddress const& addr);

protected:
  // nullopt to cache "does not exist" results
  x23::MVar<std::map<AddressAndKey, std::optional<StorageObject>>>
      storage_cache_;
  x23::MVar<std::map<SHA256Hash, std::shared_ptr<Contract>>>
      contract_cache_;
  x23::MVar<std::map<ContractAddress, std::optional<SHA256Hash>>>
      deployment_cache_;
};

namespace test {

struct LocalMockStorageSnapshot : public StorageSnapshotCache {

  SHA256Hash register_contract(std::shared_ptr<Contract> contract);

  void deploy_contract(ContractAddress const& addr, SHA256Hash const& hash);

};

} // namespace test

} // namespace hogd
