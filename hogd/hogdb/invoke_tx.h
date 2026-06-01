// invoke_tx.h - handles transactions for contract invocations

#pragma once

#include <cassert>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"
#include "hogexec_constants.hh"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "transaction.hh"

namespace hogd {

// InvokeTX represents a transaction for a root contract invocation and its
// child invocations. Rolling back nested transactions is supported through
// savepoints. The caller must provide synchronization if needed.
class InvokeTX {
public:
  InvokeTX(PerThreadARC<BlockTX> &block) : block_(block) {}
  ~InvokeTX() { abort(); }

  InvokeTX(InvokeTX &&other) noexcept = default;

  // commit commits pending changes to the BlockTX.
  HogDBStatus<void> commit();
  // abort rolls back pending changes to the BlockTX.
  void abort();

  // create_savepoint creates a savepoint of the current transaction state. It
  // returns a depth that can be used to restore to this state later.
  int create_savepoint();
  // rollback_to_savepoint restores the transaction state to a savepoint.
  void rollback_to_savepoint(int depth);
  // release_savepoint removes a savepoint and frees its associated resources.
  // If other savepoints were made after this savepoint, they will be removed
  // as well.
  void release_savepoint(int depth);

  // get_savepoint_head returns the depth of the most recent savepoint.
  unsigned int get_savepoint_head() const { return tx_depth_; }

  //
  // All of the read_* methods below read the current value of an item in just
  // this transaction relative to the snapshot state. They will return
  // kTypeMistmatch if the stored type is different from the requested type or
  // kNotFound if the item does not exist.
  //

  HogDBStatus<int128_t> read_nnint128(const ItemKey &key);
  HogDBStatus<OwningSpan<const std::byte>> read_bytes(const ItemKey &key);

  //
  // All of the request_* methods below are different operations that can be
  // performed during the first phase of two-phase commit. The request could
  // fail if there was a conflict (kConflict) or if the input was invalid
  // (e.g., kOverflow).
  //

  // request_nnint128 requests to perform set_add(base, delta).
  HogDBStatus<void> request_nnint128(const ItemKey &key, int128_t base,
                                     int128_t delta, StorageDeltaFlagSet flags);

  // request_nnint128 requests to perform set_add(SNAPSHOT.base, delta).
  HogDBStatus<void> request_nnint128(const ItemKey &key, int128_t delta,
                                     StorageDeltaFlagSet flags);

  // request_bytes requests to insert buf.
  HogDBStatus<void> request_bytes(const ItemKey &key,
                                  std::span<const std::byte> buf,
                                  StorageDeltaFlagSet flags);

  // request_deletion requests to delete the key.
  HogDBStatus<void> request_deletion(const ItemKey &key,
                                     StorageDeltaFlagSet flags);

  // request_deploy_contract requests to deploy a contract.
  // Note that this does NOT create the contract, or update a refcount on that
  // contract.
  HogDBStatus<void> request_deploy_contract(Address const &addr,
                                            const DeployedContractHash &hash);

  // request_new_contract requests to create a new contract.
  // Does NOT update the refcount on the contract.
  HogDBStatus<void> request_new_contract(Contract contract);

  // request_auth_data requests to change the auth data for an address.
  HogDBStatus<void>
  request_auth_data(Address const &addr,
                    const AuthDataChangeArgs &auth_data_change,
                    StorageDeltaFlagSet flags);
  // add uniquifier to replay cache if it is not already in the cache,
  // otherwise fail.
  HogDBStatus<void> request_consume_uniquifier(Address const &addr,
                                               Uniquifier uniquifier,
                                               StorageDeltaFlagSet flags);

  // request_retrocausal_queue requests to apply a retrocausal queue delta.
  HogDBStatus<void>
  request_retrocausal_queue(const ItemKey &key,
                            const RetrocausalQueueDelta &delta,
                            StorageDeltaFlagSet flags);

  bool release_blocktx_reference() { return block_.release_reference(); }

  void assert_is_valid() const noexcept;

  HogDBStatus<void> sync_balance_locks();

private:
  struct ItemVersion {
    ItemChange change;
    unsigned int tx_depth;
    int128_t locked_storage_cost{0};

    ItemVersion(ItemChange &&change, unsigned int tx_depth,
                int128_t locked_storage_cost = 0)
      : change(std::move(change)), tx_depth(tx_depth),
        locked_storage_cost(locked_storage_cost)
    {}

    // Disable copy and allow move.
    // This is not strictly necessary, but eliminates the possibility
    // of accidental copies that caused the *(almost) bug in PR #1196
    ItemVersion(ItemVersion &&) noexcept = default;
    ItemVersion &operator=(ItemVersion &&) noexcept = default;
  };

  struct StorageBalanceVersion {
    // The amount of storage that was manually transferred (by explicit contract
    // call) in or out of this address.
    int128_t manual_transfer_amount;
    // total locked amount at this depth,
    // for all ops on this address.
    int128_t total_locked_storage_amount;
    unsigned int tx_depth;
  };

  unsigned int tx_depth_{0};
  PerThreadARC<BlockTX> &block_;
  std::unordered_map<ItemKey, std::vector<ItemVersion>> changes_;
  // Whenever you request a balance change (from a user action),
  // combine it with the latest balance delta for that address.
  // Whenever you update a balance change from a storage operation,
  // also update the balance_deltas_ for that address.
  std::unordered_map<Address, std::vector<StorageBalanceVersion>>
      balance_deltas_;

  // read_change returns the most recent version of a change, or nullptr if
  // there is no change.
  const ItemChange *read_change(const ItemKey &key) const;

  // request_change tries to apply a change to the transaction.
  HogDBStatus<void> request_change(const ItemKey &key, ItemChange &&change);

  void request_balance_change(Address const &addr, int128_t delta);

  // lock_storage locks the storage for the given address.
  // cost must be nonnegative.
  void lock_storage(Address const &addr, int128_t cost);

  HogDBStatus<void> update_balance_lock(Address const &addr);

  // get_storage_balance_change returns the storage balance change for the given
  // address.
  HogDBStatus<Balance> get_storage_balance_change(Address const &addr) const;
};

inline int
InvokeTX::create_savepoint()
{
  CHECK(tx_depth_ < kMaxSavepointDepth);
  return ++tx_depth_;
}

inline const ItemChange *
InvokeTX::read_change(const ItemKey &key) const
{
  auto it = changes_.find(key);
  if (it == changes_.end())
    return nullptr;

  const std::vector<ItemVersion> &v = it->second;
  return &v.back().change;
}

inline HogDBStatus<void>
InvokeTX::request_bytes(const ItemKey &key, std::span<const std::byte> buf,
                        StorageDeltaFlagSet flags)
{
  auto change = ItemChange(Bytes{buf});
  if (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock)) {
    change.set_permanent_exclusive();
  }
  return request_change(key, std::move(change));
}

inline HogDBStatus<void>
InvokeTX::request_deletion(const ItemKey &key, StorageDeltaFlagSet flags)
{
  auto change = ItemChange(Deletion{});
  if (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock)) {
    change.set_permanent_exclusive();
  }
  return request_change(key, std::move(change));
}

inline HogDBStatus<void>
InvokeTX::request_deploy_contract(Address const &addr,
                                  const DeployedContractHash &hash)
{
  return request_change(make_address_metadata_item_key(addr),
                        DeployedContractData(hash));
}

} // namespace hogd
