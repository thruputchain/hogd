#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

#include "contract.hh"
#include "hogd/base/hashmap.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

class RetrocausalQueueSnapshotCacheEntry {
public:
  RetrocausalQueueSnapshotCacheEntry(DatabaseProxy &db_proxy, BlockID block_id);
  constexpr static StorageItemType kItemType =
      StorageItemType::kRetrocausalQueue;

  // only returns from past blocks
  const RetrocausalQueueBlock *
  get_next_block(AddressAndKey const &addrkey,
                 uint64_t search_start_index) const;

private:
  mutable std::map<uint64_t, std::unique_ptr<RetrocausalQueueBlock>> blocks_;
  mutable uint64_t max_observed_block_number_ = 0;
  // no need to repeatedly query for blocks above this number.
  mutable std::optional<uint64_t> block_number_upper_bound_ = std::nullopt;
  DatabaseProxy &db_proxy_;
  const BlockID block_id_;
};

class StorageSnapshotCacheEntry {
public:
  StorageSnapshotCacheEntry(const StorageItem &wire_item,
                            DatabaseProxy &db_proxy, BlockID block_id,
                            AddressAndKey const &addrkey);

  StorageSnapshotCacheEntry(const StorageSnapshotCacheEntry &) = delete;
  StorageSnapshotCacheEntry &
  operator=(const StorageSnapshotCacheEntry &) = delete;
  StorageSnapshotCacheEntry(StorageSnapshotCacheEntry &&) = delete;
  StorageSnapshotCacheEntry &operator=(StorageSnapshotCacheEntry &&) = delete;

  StorageItemType type() const;

  Bytestring const &bytestring() const;
  int128_t nnint() const;

  RetrocausalQueueSnapshotCacheEntry const &queue() const;

  std::span<const std::byte> get_key() const { return get_key(addrkey_); }

  static_assert(std::is_trivially_copyable_v<AddressAndKey>);
  static std::span<const std::byte> get_key(AddressAndKey const &addrkey)
  {
    return std::span<const std::byte, std::dynamic_extent>(
        reinterpret_cast<const std::byte *>(&addrkey), sizeof(addrkey));
  }

  const AddressAndKey &addrkey() const { return addrkey_; }

private:
  struct BytestringEntry {
    Bytestring bytestring_;
    constexpr static StorageItemType kItemType = StorageItemType::kBytestring;
  };

  struct NNIntEntry {
    int128_t base_value_;
    constexpr static StorageItemType kItemType =
        StorageItemType::kNonnegativeInt;
  };

  std::variant<BytestringEntry, NNIntEntry, RetrocausalQueueSnapshotCacheEntry>
      entry_;
  AddressAndKey addrkey_;
};

// Note: the elements of the nondeterministic concurrent read cache
// can't be part of the variant, since the type of the storage item
// might be different from the snapshot item.
// (to be explicit: an address might have no element, but then
// a queue might be read/consumed from that address).
class NondeterministicReadCacheEntry {
public:
  NondeterministicReadCacheEntry(DatabaseProxy &db_proxy, BlockID block_id,
                                 AddressAndKey addrkey)
    : db_proxy_(db_proxy), block_id_(block_id), addrkey_(addrkey)
  {}

  static std::span<const std::byte> get_key(AddressAndKey const &addrkey)
  {
    return std::span<const std::byte, std::dynamic_extent>(
        reinterpret_cast<const std::byte *>(&addrkey), sizeof(addrkey));
  }

  std::span<const std::byte> get_key() const { return get_key(addrkey_); }

  std::optional<QueueEntry> get_concurrent_queue_entry(uint64_t index) const;
  AddressAndKey const &addrkey() const { return addrkey_; }

private:
  DatabaseProxy &db_proxy_;
  BlockID block_id_;
  // If we have other types of nondeterministic reads to cache,
  // we can swap this for a variant.
  mutable std::vector<QueueEntry> concurrent_queue_entries_;
  AddressAndKey addrkey_;
};

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
  const StorageSnapshotCacheEntry *
  try_get_snapshot_item(AddressAndKey const &addrkey);

  NondeterministicReadCacheEntry &
  get_nondeterministic_read_cache_entry(AddressAndKey const &addrkey);

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
    std::unique_ptr<StorageSnapshotCacheEntry> entry_;

    static_assert(std::is_trivially_copyable_v<AddressAndKey>);
    static std::span<const std::byte> get_key(AddressAndKey const &addrkey)
    {
      return std::span<const std::byte, std::dynamic_extent>(
          reinterpret_cast<const std::byte *>(&addrkey), sizeof(addrkey));
    }
    std::span<const std::byte> get_key() const { return get_key(addrkey_); }
  };

  struct NondeterministicReadCacheMapEntry {
    AddressAndKey addrkey_;
    std::unique_ptr<NondeterministicReadCacheEntry> entry_;

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

  HashMap<NondeterministicReadCacheMapEntry> nondeterministic_read_cache_ =
      HashMap<NondeterministicReadCacheMapEntry>(8);

  const BlockID block_id_;
  DatabaseProxy &db_proxy_;
};

} // namespace hogd
