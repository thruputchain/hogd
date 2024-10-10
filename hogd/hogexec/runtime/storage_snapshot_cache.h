#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>

#include "contract.hh"
#include "storage_interface.hh"
#include "storage_object.hh"
#include "types.hh"
#include "x23/mvar.h"
#include "x23/rpc.h"

namespace hogd {

// Root cache for objects read from the ledger.
// Can be shared between concurrent transactions,
// but must be reset after each block.
class StorageSnapshotCache {
public:
  std::optional<StorageObject> try_get_object(AddressAndKey const &addrkey);
  std::shared_ptr<Contract> try_get_contract_by_hash(SHA256Hash const &hash);
  std::optional<SHA256Hash>
  try_get_deployed_contract_hash(ContractAddress const &addr);

  StorageSnapshotCache(
      x23::RpcStream<x23::RpcLogger>::Configurator &configurator)
    : client_(configurator.template client<StorageReadInterfaceV1>())
  {}

private:
  // TODO(geoff): initialize and check this properly
  SHA256Hash current_block_hash_;

  // nullopt to cache "does not exist" results
  x23::MVar<std::map<AddressAndKey, std::optional<StorageObject>>>
      storage_cache_;
  x23::MVar<std::map<SHA256Hash, std::shared_ptr<Contract>>> contract_cache_;
  x23::MVar<std::map<ContractAddress, std::optional<SHA256Hash>>>
      deployment_cache_;

  StorageReadInterfaceV1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>
      client_;
};

} // namespace hogd
