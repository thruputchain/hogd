#pragma once

#include <set>
#include <unordered_map>
#include <vector>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/mvar.h"
#include "x23/threadpool.h"

namespace hogd {

// Serve rpcs for runtime tests.
//
// Initialize manually with database snapshot state.
//
// Eventually, once the DB is merged in, this may get replaced
// by a full mock database.
class BlockSnapshotServer {
public:
  using rpc_service = DatabaseToExecutorInterfaceV1;

  BlockSnapshotServer(BlockID block_id) : block_id_(block_id) {}

  static x23::Async<void> db_snapshot_read_null();

  static x23::Async<RPCError> commit_transaction(BlockID block,
                                                 TransactionID tx);
  static x23::Async<RPCError> abort_transaction(BlockID block,
                                                TransactionID tx);

  // Create a new subtransaction on top of the current (sub)transaction.
  static x23::Async<RPCError>
  try_push_new_subtransaction(BlockID block, TransactionID tx,
                              StorageDeltaBatch batch);

  // Revert one or more subtransactions.
  static x23::Async<RPCError>
  pop_subtransactions(BlockID block, TransactionID tx, uint32_t popcount);

  static x23::Async<RPCError> commit_contract_create(BlockID block,
                                                     Contract contract);
  static x23::Async<RPCError>
  commit_contract_deploy(BlockID block, ContractAddress const &contract_addr,
                         SHA256Hash const &hash);

  x23::Async<ExpectedStorageItem>
  get_snapshot_item(BlockID block, AddressAndKey const &addrkey);
  x23::Async<ExpectedContract> get_contract(BlockID block,
                                            SHA256Hash const &contract_hash);
  x23::Async<ExpectedDeployment>
  get_deployment(BlockID block, ContractAddress const &contract_address);

  x23::Async<ExpectedRetrocausalQueueBlock>
  get_next_retrocausal_queue_block(BlockID block, AddressAndKey const &addrkey,
                                   uint64_t search_start_number);

  x23::Async<ExpectedQueueEntry> get_concurrent_retrocausal_queue_entry(
      BlockID block, AddressAndKey const &addrkey, uint32_t index);

  sockaddr_storage start_server(uint32_t port_number);

  // Only used within test setup
  void setup_create_contract(Contract const &contract);
  void setup_create_deployment(ContractAddress addr, SHA256Hash contract_hash);
  void setup_create_item(AddressAndKey const &addrkey, StorageItemWrapper item);
  void setup_add_concurrent_rcq_item(AddressAndKey const &addrkey,
                                     QueueEntry entry);

  void setup_ban_item(AddressAndKey const &addrkey);

private:
  BlockID block_id_;

  x23::ThreadPool pool_ = x23::ThreadPool(2);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  x23::MVar<std::unordered_map<AddressAndKey, StorageItemWrapper>> items_;
  x23::MVar<std::unordered_map<SHA256Hash, Contract>> contracts_;
  x23::MVar<std::unordered_map<ContractAddress, SHA256Hash>> deployments_;
  x23::MVar<std::unordered_map<AddressAndKey, std::vector<QueueEntry>>>
      concurrent_rcq_items_;

  // For testing lazy loading:
  // Creates a set of items that throw an error if they are accessed at all
  x23::MVar<std::set<AddressAndKey>> blocked_items_;
};

static_assert(
    is_DatabaseToExecutorInterfaceV1<BlockSnapshotServer *, x23::Async>);

} // namespace hogd
