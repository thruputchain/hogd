#pragma once

#include <cassert>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "contract.hh"
#include "coord.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"

namespace hogd {

struct LocalMockDB {
  uint32_t read_port_number_;
};

struct ShardedDB {
  ShardedDB(IntroExecResult const &coord_info)
    : shards_(coord_info.shards.begin(), coord_info.shards.end())
  {}

  std::vector<ShardCard> shards_;
};

// Information identifying where and how to access the database.
// LocalMockDB redirects RPC calls to a server on localhost.
using DatabaseIdentifier = std::variant<LocalMockDB, ShardedDB>;

// Abstract away database sharding from the rest of the executor.
// Handles the RPC Error codes, changing them into restricted forms
// that the executor can handle (or abort on).
class DatabaseProxy {
public:
  DatabaseProxy(x23::CoroPoll &cp, DatabaseIdentifier identifier);

  DatabaseProxy(DatabaseProxy const &) = delete;
  DatabaseProxy &operator=(DatabaseProxy const &) = delete;
  DatabaseProxy(DatabaseProxy &&) = delete;
  DatabaseProxy &operator=(DatabaseProxy &&) = delete;

  x23::Async<SystemStatus<std::optional<StorageItem>>>
  get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey);
  x23::Async<SystemStatus<std::shared_ptr<const Contract>>>
  get_contract(BlockID block_id, SHA256Hash const &contract_hash);
  x23::Async<SystemStatus<std::optional<SHA256Hash>>>
  get_deployment(BlockID block_id, ContractAddress const &addr);

  x23::Async<SystemStatus<std::optional<RetrocausalQueueBlock>>>
  get_next_retrocausal_queue_block(BlockID block_id,
                                   AddressAndKey const &addrkey,
                                   uint64_t search_start_number);

  x23::Async<SystemStatus<std::optional<QueueEntry>>>
  get_concurrent_retrocausal_queue_entry(BlockID block_id,
                                         AddressAndKey const &addrkey,
                                         uint32_t index);

  x23::Async<SystemStatus<bool>>
  try_push_new_subtransaction(BlockID block_id, TransactionID tx_id,
                              StorageDeltaBatch const &delta_batch);

  x23::Async<SystemStatus<void>> commit_transaction(BlockID block_id,
                                                    TransactionID tx_id);
  x23::Async<SystemStatus<void>> abort_transaction(BlockID block_id,
                                                   TransactionID tx_id);

  x23::Async<SystemStatus<void>> commit_contract_create(BlockID block_id,
                                                        Contract const& contract);
  x23::Async<SystemStatus<void>> commit_contract_deploy(BlockID block_id,
                                                        ContractAddress const& addr,
                                                        SHA256Hash const& hash);

  x23::Async<SystemStatus<void>> pop_subtransactions(BlockID block_id,
                                                     TransactionID tx_id,
                                                     uint32_t count = 1);
  x23::CoroPoll &coropoll() { return cp_; }

private:
  x23::CoroPoll &cp_;
  using read_client_t = DatabaseToExecutorInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  read_client_t read_client_;
};

class StorageDeltaBatchBuilder {
public:
  // Is responsible for sorting storage deltas appropriately for consumption by
  // the database.
  StorageDeltaBatch build();

  void add_delta(AddressAndKey const &addrkey, StorageDelta const &delta,
                 StorageItemRPCFlagSet flags);

private:
  std::vector<FlaggedStorageDelta> positive_storage_deltas_;
  std::vector<FlaggedStorageDelta> negative_storage_deltas_;

  // might be positive or negative; we don't know in executor which is the case
  std::vector<FlaggedStorageDelta> storage_deltas_;
};

// A DatabaseProxy that is scoped to a single transaction.
class PerTransactionDatabaseProxy {
public:
  PerTransactionDatabaseProxy(DatabaseProxy &db_proxy, BlockID block_id,
                              TransactionID tx_id)
    : db_proxy_(db_proxy), block_id_(block_id), tx_id_(tx_id)
  {}

  std::optional<StorageDeltaBatch> current_subtransaction_batch_;

  void add_delta(AddressAndKey const &addrkey, StorageDelta const &delta,
                 StorageItemRPCFlagSet flags)
  {
    if (!current_subtransaction_batch_builder_) {
      current_subtransaction_batch_builder_ = StorageDeltaBatchBuilder();
    }
    current_subtransaction_batch_builder_->add_delta(addrkey, delta, flags);
  }

  x23::Async<SystemStatus<bool>> try_push_current_subtransaction()
  {
    StorageDeltaBatch batch;
    if (current_subtransaction_batch_builder_) {
      batch = current_subtransaction_batch_builder_->build();
      current_subtransaction_batch_builder_ = std::nullopt;
    }

    return db_proxy_.try_push_new_subtransaction(block_id_, tx_id_, batch);
  }

  x23::Async<SystemStatus<void>> pop_subtransactions(uint32_t count)
  {
    return db_proxy_.pop_subtransactions(block_id_, tx_id_, count);
  }

  x23::Async<SystemStatus<void>> commit_transaction()
  {
    assert(!current_subtransaction_batch_builder_);
    return db_proxy_.commit_transaction(block_id_, tx_id_);
  }

  x23::Async<SystemStatus<void>> abort_transaction()
  {
    assert(!current_subtransaction_batch_builder_);
    return db_proxy_.abort_transaction(block_id_, tx_id_);
  }

  x23::Async<SystemStatus<void>> commit_contract_create(Contract const& contract)
  {
    assert(!current_subtransaction_batch_builder_);
    return db_proxy_.commit_contract_create(block_id_, contract);
  }

  x23::Async<SystemStatus<void>> commit_contract_deploy(ContractAddress const& addr, SHA256Hash const& hash)
  {
    assert(!current_subtransaction_batch_builder_);
    return db_proxy_.commit_contract_deploy(block_id_, addr, hash);
  }

  x23::CoroPoll &coropoll() { return db_proxy_.coropoll(); }

private:
  DatabaseProxy &db_proxy_;
  std::optional<StorageDeltaBatchBuilder>
      current_subtransaction_batch_builder_ = std::nullopt;
  const BlockID block_id_;
  const TransactionID tx_id_;
};
} // namespace hogd
