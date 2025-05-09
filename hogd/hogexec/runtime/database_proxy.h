#pragma once

#include <cassert>
#include <memory>
#include <optional>
#include <variant>

#include "contract.hh"
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

// Information identifying where and how to access the database.
// LocalMockDB redirects RPC calls to a server on localhost.
using DatabaseIdentifier = std::variant<LocalMockDB>;

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

  x23::Async<SystemStatus<std::optional<int128_t>>>
  try_reserve_delta(BlockID block_id, TransactionID tx_id,
                    AddressAndKey const &addrkey, StorageDelta const &delta,
                    StorageItemRPCFlagSet flags);

  x23::Async<SystemStatus<void>> commit_transaction(BlockID block_id,
                                                    TransactionID tx_id);
  x23::Async<SystemStatus<void>> abort_transaction(BlockID block_id,
                                                   TransactionID tx_id);

  x23::Async<SystemStatus<void>> push_new_subtransaction(BlockID block_id,
                                                         TransactionID tx_id);
  x23::Async<SystemStatus<void>> pop_subtransactions(BlockID block_id,
                                                     TransactionID tx_id,
                                                     uint32_t count = 1);

private:
  using read_client_t = DatabaseToExecutorInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  read_client_t read_client_;
};

// A DatabaseProxy that is scoped to a single transaction.
class PerTransactionDatabaseProxy {
public:
  PerTransactionDatabaseProxy(DatabaseProxy &db_proxy, BlockID block_id,
                              TransactionID tx_id)
    : db_proxy_(db_proxy), block_id_(block_id), tx_id_(tx_id)
  {}

  x23::Async<SystemStatus<std::optional<int128_t>>>
  try_reserve_delta(AddressAndKey const &addrkey, StorageDelta const &delta,
                    StorageItemRPCFlagSet flags)
  {
    return db_proxy_.try_reserve_delta(block_id_, tx_id_, addrkey, delta,
                                       flags);
  }

  x23::Async<SystemStatus<void>> push_new_subtransaction()
  {
    return db_proxy_.push_new_subtransaction(block_id_, tx_id_);
  }

  x23::Async<SystemStatus<void>> pop_subtransactions(uint32_t count)
  {
    return db_proxy_.pop_subtransactions(block_id_, tx_id_, count);
  }

private:
  DatabaseProxy &db_proxy_;
  const BlockID block_id_;
  const TransactionID tx_id_;
};
} // namespace hogd
