#pragma once

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

#include "contract.hh"
#include "hogd/base/hashmap.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/mvar.h"

namespace hogd {

// Root cache for objects read from the ledger.
// Can be shared between concurrent transactions,
// but must be reset after each block.
// Should not be used for transactions in different blocks.
//
// This is not designed as a high performance implementation.
// If this is a performance bottleneck, it can be swapped out
// for something with this same interface.
class StorageSnapshotCache {
public:
  StorageSnapshotCache(BlockID block_id, DatabaseProxy &db_proxy)
    : block_id_(block_id), db_proxy_(db_proxy)
  {}

  std::shared_ptr<const Contract>
  try_get_contract_by_hash(SHA256Hash const &hash);
  std::optional<SHA256Hash>
  try_get_deployed_contract_hash(ContractAddress const &addr);
  const StorageItem *try_get_snapshot_item(AddressAndKey const &addrkey);

private:
  struct ContractMapEntry {
    SHA256Hash hash_;
    std::shared_ptr<const Contract> contract_;

    static std::span<const std::byte> get_key(SHA256Hash const &hash)
    {
      return std::as_bytes(std::span(hash.data(), hash.size()));
    }
    std::span<const std::byte> get_key() const { return get_key(hash_); }
  };

  struct DeploymentMapEntry {
    ContractAddress addr_;
    std::optional<SHA256Hash> hash_;

    static std::span<const std::byte> get_key(ContractAddress const &addr)
    {
      return std::as_bytes(std::span(addr.data(), addr.size()));
    }
    std::span<const std::byte> get_key() const { return get_key(addr_); }
  };

  struct StorageItemMapEntry {
    AddressAndKey addrkey_;
    std::unique_ptr<StorageItem> item_;

    static_assert(std::is_trivially_copyable_v<AddressAndKey>);
    static std::span<const std::byte> get_key(AddressAndKey const &addrkey)
    {
      return std::span<const std::byte, std::dynamic_extent>(
          reinterpret_cast<const std::byte *>(&addrkey), sizeof(addrkey));
    }
    std::span<const std::byte> get_key() const { return get_key(addrkey_); }
  };

  // Note: the contract_cache_ does not strictly need to be reset (although it
  // is not incorrect to reset it) after every block.
  HashMap<ContractMapEntry> contract_cache_ = HashMap<ContractMapEntry>(8);
  // Neither does the deployment cache
  HashMap<DeploymentMapEntry> deployment_cache_ =
      HashMap<DeploymentMapEntry>(8);
  // snapshot cache needs to be reset after each block
  HashMap<StorageItemMapEntry> snapshot_item_cache_ =
      HashMap<StorageItemMapEntry>(8);

  const BlockID block_id_;
  DatabaseProxy &db_proxy_;
};

} // namespace hogd
