#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include "storage_object.hh"
#include "types.hh"
#include "x23/mvar.h"

namespace hogd {

// Root cache for objects read from the ledger.
// Can be shared between concurrent transactions,
// but must be reset after each block.
class StorageSnapshotCache {
public:
  std::optional<StorageObject> try_get_object(AddressAndKey const &addrkey);
  std::shared_ptr<Contract>
  try_get_contract_by_addr(ContractAddress const &addr);

private:
  // nullopt to cache "does not exist" results
  x23::MVar<std::unordered_map<AddressAndKey, std::optional<StorageObject>>>
      storage_cache_;
  x23::MVar<std::unordered_map<ContractAddress, std::shared_ptr<Contract>>>
      contract_cache_;
};

} // namespace hogd
