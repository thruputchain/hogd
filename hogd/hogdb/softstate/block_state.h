#pragma once

#include <memory>

#include "hogd/hogdb/db/in_memory_database.h"
#include "hogd/hogdb/softstate/storage_view.h"
#include "hogd/hogdb/softstate/transaction_cache.h"
#include "rpc_types.hh"

namespace hogd {

class BlockState {
public:
  BlockState(std::shared_ptr<BlockState> parent)
    : storage_view_(&(parent->storage_view_)), parent_state_(std::move(parent))
  {}

  BlockState(InMemoryDatabase *base_db)
    : storage_view_(base_db), parent_state_(nullptr)
  {}

  BlockState(BlockState const &) = delete;
  BlockState &operator=(BlockState const &) = delete;
  BlockState(BlockState &&) = delete;
  BlockState &operator=(BlockState &&) = delete;

  void unlink_parent(InMemoryDatabase *base_database);

  // It is an error to reserve a transaction delta for a txid and then
  // not call commit_transaction or abort_transaction before
  // ending the block.
  bool try_reserve_storage_delta(TransactionID txid,
                                 AddressAndKey const &addrkey,
                                 StorageDelta const &delta);
  void commit_transaction(TransactionID txid);
  void abort_transaction(TransactionID txid);

private:
  StorageView storage_view_;

  TransactionCache transaction_cache_;

  // nullptr if parent has been unlinked
  // Hold this shared_ptr to keep parent state alive.
  std::shared_ptr<BlockState> parent_state_;
};

} // namespace hogd
