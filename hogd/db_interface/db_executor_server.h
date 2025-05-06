#pragma once

#include <cassert>
#include <memory>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/types.h"
#include "hogd/db_interface/block_manager.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

// Async server implementing the DatabaseToExecutorInterfaceV1 interface
// for handling database operations from the executor.
class DbExecutorServer {
public:
  using rpc_service = DatabaseToExecutorInterfaceV1;

  explicit DbExecutorServer(BlockManager *block_manager)
    : block_manager_(block_manager)
  {
    assert(block_manager_);
  }

  // Start the server on the specified port
  void start_server(uint32_t port_number);

  // RPC methods
  x23::Async<ExpectedStorageCost>
  try_reserve_delta(BlockID block, TransactionID tx, AddressAndKey addrkey,
                    StorageDelta delta, StorageItemRPCFlagSet flags);
  x23::Async<void> commit_transaction(BlockID block, TransactionID tx);
  x23::Async<void> abort_transaction(BlockID block, TransactionID tx);
  x23::Async<RPCError> push_new_subtransaction(BlockID block, TransactionID tx);
  x23::Async<RPCError> pop_subtransactions(BlockID block, TransactionID tx,
                                           uint32_t popcount);
  x23::Async<ExpectedStorageItem>
  get_snapshot_item(BlockID block, AddressAndKey const &addrkey);
  x23::Async<void> commit_contract_create(BlockID block, Contract contract);
  x23::Async<void> commit_contract_deploy(BlockID block,
                                          ContractAddress const &addr,
                                          SHA256Hash const &hash);
  x23::Async<ExpectedContract> get_contract(BlockID block,
                                            SHA256Hash const &contract_hash);
  x23::Async<ExpectedDeployment>
  get_deployment(BlockID block, ContractAddress const &contract_address);

  // RCQ operations
  x23::Async<ExpectedRetrocausalQueueBlock>
  get_next_retrocausal_queue_block(BlockID block, AddressAndKey const &addrkey,
                                   uint64_t search_start_number);

  x23::Async<ExpectedQueueEntry> get_concurrent_retrocausal_queue_entry(
      BlockID block, AddressAndKey const &addrkey, uint32_t index);

private:
  BlockManager *block_manager_;
  x23::ThreadPool pool_ = x23::ThreadPool(2);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);
};

static_assert(is_DatabaseToExecutorInterfaceV1<DbExecutorServer *, x23::Async>);

} // namespace hogd
