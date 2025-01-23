#pragma once

#include <memory>
#include <optional>

#include "contract.hh"
#include "hogd/hogdb/db/in_memory_database.h"
#include "hogd/hogdb/softstate/storage_view.h"
#include "hogd/hogdb/softstate/transaction_cache.h"
#include "rpc_types.hh"

namespace hogd {

// Wrapper class to manage all of the state associated with one
// (pending or committed) block.
// Holds reference to a parent block as long as necessary;
// when the parent block is eventually committed to the database,
// the parent can be unlinked, at which point the reference is released.
//
// Note:  As implemented,
// the base database does not maintain versioning for blocks that do not commit
// and have no possibility of committing (i.e., some directly preceeding
// block had its sibling commit instead of it).
// Any pending modifications on this block (from, e.g., delayed RPCs or slow
// executors) may read invalid state (and give invalid try_reserve_storage_delta
// results) as a result.  However, these blocks will never commit anyways, so
// this does not affect correctness.
class BlockState {
public:
  BlockState(std::shared_ptr<BlockState> parent)
    : storage_view_(parent->storage_view_.as_parent()),
      parent_state_(std::move(parent))
  {}

  BlockState(InMemoryDatabase *base_db)
    : storage_view_(base_db), parent_state_(nullptr)
  {
    // This is a hack to work around the fact that the initial block starts as
    // committed, even though the initial block is empty. BlockManager needs a
    // last_committed_id_. Alternative is to make last_committed_id_ optional,
    // and have every commit do an appropriate check. This seemed simpler.
    storage_view_.close_block();
  }

  BlockState(BlockState const &) = delete;
  BlockState &operator=(BlockState const &) = delete;
  BlockState(BlockState &&) = delete;
  BlockState &operator=(BlockState &&) = delete;

  // Unlinks this block's parent state.  Does not modify the previous parent
  // state, so the previous parent state must be already committed to the base
  // database. After unlinking, this block will read directly from the base
  // database instead of the parent. The parent block state will be destroyed
  // once all references to it are released.
  void unlink_parent(InMemoryDatabase *base_database);

  // It is an error to reserve a transaction delta for a txid and then
  // not call commit_transaction or abort_transaction before
  // ending the block.
  // Requirement: A transaction cannot try to reserve multiple deltas for the
  // same addrkey at the same time.  This is not enforced by the interface.
  bool try_reserve_storage_delta(TransactionID txid,
                                 AddressAndKey const &addrkey,
                                 StorageDelta const &delta, RPCFlagSet flags);
  void commit_transaction(TransactionID txid);
  void abort_transaction(TransactionID txid);

  void commit_contract_create(Contract const &contract);
  void commit_contract_deploy(ContractAddress const &addr,
                              SHA256Hash const &contract_hash);

  std::optional<StorageItem>
  get_snapshot_storage_item(AddressAndKey const &addrkey);
  std::optional<SHA256Hash>
  get_snapshot_deployment(ContractAddress const &addr);
  std::shared_ptr<const Contract>
  get_snapshot_contract(SHA256Hash const &contract_hash);

  void close_block() { storage_view_.close_block(); }
  void assert_closed() { storage_view_.assert_block_closed(); }

private:
  StorageView storage_view_;
  TransactionCache transaction_cache_;

  // nullptr if parent has been unlinked
  // Hold this shared_ptr to keep parent state alive.
  std::shared_ptr<BlockState> parent_state_;
};

} // namespace hogd
