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
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/status.h"

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

private:
  struct ItemVersion {
    ItemChange change;
    int tx_depth;
  };

  int tx_depth_{0};
  BlockTX &block_;
  std::unordered_map<ItemKey, std::vector<ItemVersion>> changes_;

  // read_change returns the most recent version of a change, or nullptr if
  // there is no change.
  const ItemChange *read_change(const ItemKey &key) const;
  // request_change tries to apply a change to the transaction.
  HogDBStatus<void> request_change(const ItemKey &key, ItemChange &&change);

  void assert_is_valid() const
  {
    assert(tx_depth_ >= 0);
    if constexpr (is_debug_build()) {
      for (const auto &[key, vec] : changes_) {
        [[maybe_unused]] int depth = 0;
        for (const ItemVersion &item : vec) {
          assert(item.tx_depth >= depth);
          assert(item.tx_depth <= tx_depth_);
          depth = item.tx_depth + 1;
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
  return request_change(make_address_metadata_item_key(addr),
                        DeployedContractData(hash));
}

inline HogDBStatus<void>
InvokeTX::request_new_contract(Contract contract)
{
  auto hash = sha256(contract);
  return request_change(make_contract_code_item_key(hash),
                        NewContractData(std::move(contract)));
}

} // namespace hogd
