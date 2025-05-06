#pragma once

#include <cassert>
#include <cstdint>
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
class DatabaseProxy {
public:
  DatabaseProxy(x23::CoroPoll &cp, DatabaseIdentifier identifier);

  DatabaseProxy(DatabaseProxy const &) = delete;
  DatabaseProxy &operator=(DatabaseProxy const &) = delete;
  DatabaseProxy(DatabaseProxy &&) = delete;
  DatabaseProxy &operator=(DatabaseProxy &&) = delete;

  x23::Async<HostFnStatus<std::unique_ptr<const StorageItem>>>
  get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey);
  x23::Async<HostFnStatus<std::shared_ptr<const Contract>>>
  get_contract(BlockID block_id, SHA256Hash const &contract_hash);
  x23::Async<HostFnStatus<std::optional<SHA256Hash>>>
  get_deployment(BlockID block_id, ContractAddress const &addr);

  x23::Async<HostFnStatus<std::optional<RetrocausalQueueBlock>>>
  get_next_retrocausal_queue_block(BlockID block_id,
                                   AddressAndKey const &addrkey,
                                   uint64_t search_start_number);

  x23::Async<HostFnStatus<std::optional<QueueEntry>>>
  get_concurrent_retrocausal_queue_entry(BlockID block_id,
                                         AddressAndKey const &addrkey,
                                         uint32_t index);

  x23::Async<HostFnStatus<std::optional<int128_t>>>
  try_reserve_delta(BlockID block_id, TransactionID tx_id,
                    AddressAndKey const &addrkey, StorageDelta const &delta,
                    StorageItemRPCFlagSet flags);

  x23::Async<void> commit_transaction(BlockID block_id, TransactionID tx_id);
  x23::Async<void> abort_transaction(BlockID block_id, TransactionID tx_id);

private:
  using read_client_t = DatabaseToExecutorInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  read_client_t read_client_;
};

} // namespace hogd
