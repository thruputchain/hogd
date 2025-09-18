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
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/srpc.h"

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
  DatabaseProxy(DatabaseIdentifier identifier);

  DatabaseProxy(DatabaseProxy const &) = delete;
  DatabaseProxy &operator=(DatabaseProxy const &) = delete;
  DatabaseProxy(DatabaseProxy &&) = delete;
  DatabaseProxy &operator=(DatabaseProxy &&) = delete;

  SystemStatus<std::optional<StorageItem>>
  get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey);

  SystemStatus<std::shared_ptr<const Contract>>
  get_contract(BlockID block_id, SHA256Hash const &contract_hash);

  SystemStatus<std::optional<SHA256Hash>>
  get_deployment(BlockID block_id, ContractAddress const &addr);

  SystemStatus<std::optional<RetrocausalQueueBlock>>
  get_next_retrocausal_queue_block(BlockID block_id,
                                   AddressAndKey const &addrkey,
                                   uint64_t search_start_number);

  SystemStatus<std::optional<QueueEntry>>
  get_concurrent_retrocausal_queue_entry(BlockID block_id,
                                         AddressAndKey const &addrkey,
                                         uint32_t index);

  SystemStatus<bool>
  try_push_new_subtransaction(BlockID block_id, TransactionID tx_id,
                              StorageDeltaBatch const &delta_batch);

  SystemStatus<void> commit_transaction(BlockID block_id, TransactionID tx_id, NewContractBatch const& new_contracts);

  SystemStatus<void> abort_transaction(BlockID block_id, TransactionID tx_id);

  SystemStatus<void> pop_subtransactions(BlockID block_id, TransactionID tx_id,
                                         uint32_t count);

private:
  using base_client_t = x23::SRpcMulticlient<x23::RpcLogger>::ClientRef;
  using client_t = DatabaseToExecutorInterfaceV1::client<base_client_t>;
  client_t client_;
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

  SystemStatus<bool> try_push_current_subtransaction()
  {
    StorageDeltaBatch batch;
    if (current_subtransaction_batch_builder_) {
      batch = current_subtransaction_batch_builder_->build();
      current_subtransaction_batch_builder_ = std::nullopt;
    }

    return db_proxy_.try_push_new_subtransaction(block_id_, tx_id_, batch);
  }

  SystemStatus<void> pop_subtransactions(uint32_t count)
  {
    return db_proxy_.pop_subtransactions(block_id_, tx_id_, count);
  }

  SystemStatus<void> commit_transaction(NewContractBatch const& new_contracts)
  {
    assert(!current_subtransaction_batch_builder_);
    return db_proxy_.commit_transaction(block_id_, tx_id_, new_contracts);
  }

  SystemStatus<void> abort_transaction()
  {
    assert(!current_subtransaction_batch_builder_);
    return db_proxy_.abort_transaction(block_id_, tx_id_);
  }

private:
  DatabaseProxy &db_proxy_;
  std::optional<StorageDeltaBatchBuilder>
      current_subtransaction_batch_builder_ = std::nullopt;
  const BlockID block_id_;
  const TransactionID tx_id_;
};
} // namespace hogd
