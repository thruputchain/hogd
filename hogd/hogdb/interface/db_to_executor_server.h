#pragma once

#include "hogd/hogdb/interface/block_manager.h"
#include "rpc.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class DBToExecutorServer {
public:
  using rpc_service = DatabaseToExecutorInterfaceV1;

  DBToExecutorServer(BlockManager &block_manager)
    : block_manager_(block_manager)
  {}

  x23::Async<RPCError> try_reserve_delta(BlockID block_id, TransactionID txid,
                                         AddressAndKey const &addrkey,
                                         StorageDelta const &delta,
                                         RPCFlagSet flags);
  x23::Async<void> commit_transaction(BlockID block_id, TransactionID txid);
  x23::Async<void> abort_transaction(BlockID block_id, TransactionID txid);

  x23::Async<ExpectedStorageItem> get_item(BlockID block_id,
                                           AddressAndKey const &addrkey);

  x23::Async<ExpectedRetrocausalQueueBlock> get_next_retrocausal_queue_block(
      BlockID block_id, AddressAndKey const &addrkey, uint64_t block_number);

  x23::Async<ExpectedQueueEntry> get_concurrent_retrocausal_queue_entry(
      BlockID block_id, AddressAndKey const &addrkey, uint32_t index);

  x23::Async<void> commit_contract_create(BlockID block_id,
                                          Contract const &contract);
  x23::Async<void> commit_contract_deploy(BlockID block_id,
                                          ContractAddress const &addr,
                                          SHA256Hash const &contract_hash);
  x23::Async<ExpectedContract> get_contract(BlockID block_id,
                                            SHA256Hash const &contract_hash);
  x23::Async<ExpectedDeployment> get_deployment(BlockID block_id,
                                                ContractAddress const &addr);

private:
  BlockManager &block_manager_;
};

static_assert(
    is_DatabaseToExecutorInterfaceV1<DBToExecutorServer *, x23::Async>);

} // namespace hogd
