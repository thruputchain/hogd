// invoke_tx.h - handles transactions for contract invocations

#pragma once

#include <cassert>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "account.hh"
#include "contract.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"
#include "storage_item.hh"

namespace hogd {

// InvokeTX represents a transaction for a root contract invocation and its
// child invocations. Rolling back nested transactions is supported through
// savepoints. The caller must provide synchronization if needed.
class InvokeTX {
public:
  InvokeTX(BlockTX &block) : block_(block) {}
  ~InvokeTX() { abort(); }

  // commit commits pending changes to the BlockTX.
  void commit();
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
  int get_savepoint_head() const { return tx_depth_; }

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
                                     int128_t delta);

  // request_nnint128 requests to perform set_add(SNAPSHOT.base, delta).
  HogDBStatus<void> request_nnint128(const ItemKey &key, int128_t delta);

  // request_bytes requests to insert buf.
  HogDBStatus<void> request_bytes(const ItemKey &key,
                                  std::span<const std::byte> buf);

  // request_deletion requests to delete the key.
  HogDBStatus<void> request_deletion(const ItemKey &key);

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
                    const AuthDataChangeArgs &auth_data_change);

private:
  struct ItemVersion {
    ItemChange change;
    int tx_depth;
    int128_t locked_storage_cost{0};
  };

  struct StorageBalanceVersion {
    // The amount of storage that was manually transferred (by explicit contract
    // call) in or out of this address.
    int128_t manual_transfer_amount;
    // total locked amount at this depth,
    // for all ops on this address.
    int128_t total_locked_storage_amount;
    int tx_depth;
  };

  int tx_depth_{0};
  BlockTX &block_;
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
  HogDBStatus<void> request_balance_change(Address const &addr, int128_t delta);

  // lock_storage locks the storage for the given address.
  // cost must be nonnegative.
  HogDBStatus<void> lock_storage(Address const &addr, int128_t cost);

  HogDBStatus<void> update_balance_lock(Address const &addr);
  HogDBStatus<NNInt128> get_storage_balance_change(Address const &addr);

  void assert_is_valid() const
  {
    assert(tx_depth_ >= 0);
    std::unordered_map<Address, int128_t> balance_deltas;
    if constexpr (is_debug_build()) {
      for (const auto &[key, vec] : changes_) {
        [[maybe_unused]] int depth = 0;
        for (const ItemVersion &item : vec) {
          assert(item.tx_depth >= depth);
          assert(item.tx_depth <= tx_depth_);
          depth = item.tx_depth + 1;
        }
        if (!vec.empty()) {
          balance_deltas[key.get_address()] += vec.back().locked_storage_cost;
        }
      }
      for (const auto &[addr, vec] : balance_deltas_) {
        if (!vec.empty()) {
          assert(vec.back().total_locked_storage_amount ==
                 balance_deltas[addr]);
        }
        else {
          assert(balance_deltas[addr] == 0);
        }
      }
    }
  }
};

inline int
InvokeTX::create_savepoint()
{
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
InvokeTX::request_bytes(const ItemKey &key, std::span<const std::byte> buf)
{
  return request_change(key, Bytes{buf});
}

inline HogDBStatus<void>
InvokeTX::request_deletion(const ItemKey &key)
{
  return request_change(key, Deletion{});
}

inline HogDBStatus<void>
InvokeTX::request_deploy_contract(Address const &addr,
                                  const DeployedContractHash &hash)
{
  // The executor should block redeployments of contracts that are already
  // deployed.
  if (auto r = block_.get_db().read_address_metadata(addr)) {
    if (r.has_value()) {
      error("attempted to redeploy contract that is already deployed on "
            "address {}",
            addr);
      return HogDBError(HogDBCode::kInternal);
    }
  }
  return request_change(make_address_metadata_item_key(addr),
                        DeployedContractData(hash));
}

inline HogDBStatus<void>
InvokeTX::request_new_contract(Contract contract)
{
  // TODO(geoff): For some reason, on gcc in CI (not clang locally)
  // doing the sha256(contract) within the make_contract_code_item_key
  // function results in what appears to be some kind of memory corruption.
  // Address sanitizer didn't flag anything, however.
  auto hash = sha256(contract);
  return request_change(make_contract_code_item_key(hash),
                        NewContractData(std::move(contract)));
}

} // namespace hogd
