#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <type_traits>

// generated x23 headers
#include "db_conflict.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/hog_db/invoke_tx.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/types.h"
#include "hogd/hog_db/rpc_context.h"

namespace hogd {

// Core logic handler for the db rpc server.
class HogDbExecutorRpc {
public:
  using rpc_service = DatabaseToExecutorInterfaceV1;

  explicit HogDbExecutorRpc(RPCContext &ctx) : ctx_(ctx) {}

  RPCError commit_transaction(BlockID block, TransactionID tx,
                              NewContractBatch new_contracts);
  RPCError abort_transaction(BlockID block, TransactionID tx);
  TryPushSubtransactionResult
  try_push_new_subtransaction(BlockID block, TransactionID tx,
                              StorageDeltaBatch batch);
  RPCError pop_subtransactions(BlockID block, TransactionID tx,
                               uint32_t popcount);

  ExpectedStorageItem get_snapshot_item(BlockID block,
                                        AddressAndKey const &addrkey);

  ExpectedContract get_contract(BlockID block, SHA256Hash const &hash);
  ExpectedDeployment get_deployment(BlockID block,
                                    ContractAddress const &contract_address);
  ExpectedRetrocausalQueueBlock
  get_next_retrocausal_queue_block(BlockID block, AddressAndKey const &addrkey,
                                   uint64_t search_start_number);
  ExpectedQueueEntry get_concurrent_retrocausal_queue_entry(
      BlockID block, AddressAndKey const &addrkey, uint32_t index);

  ExpectedDBTip get_db_tip(BlockID block);

  ExpectedViewingSnapshot open_viewing_snapshot();
  RPCError close_viewing_snapshot(BlockID block);

  void shut_down()
  {
    // TODO(jhumphri): Implement.
    shutdown_flag_ = true;
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
  RPCContext &ctx_;

  static RPCError request_one(InvokeTX &tx, AddressAndKey const &addrkey,
                              StorageDelta &&delta, StorageDeltaFlagSet flags,
                              DbConflictCode *conflict_code);
};
static_assert(
    is_DatabaseToExecutorInterfaceV1<HogDbExecutorRpc *, std::type_identity_t>);

class HogDbExecutorServer : public RpcServer<HogDbExecutorRpc> {
public:
  explicit HogDbExecutorServer(RPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
