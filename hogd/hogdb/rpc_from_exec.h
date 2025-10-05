#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <thread>
#include <type_traits>

// generated x23 headers
#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/hogdb/invoke_tx.h"
#include "rpc_types.hh"
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/rpc_context.h"
#include "x23/cleanup.h"

namespace hogd {

// Core logic handler for the db rpc server.
class HogDBExecutorRpc {
public:
  using rpc_service = DatabaseToExecutorInterfaceV1;

  explicit HogDBExecutorRpc(RPCContext &ctx) : ctx_(ctx) {}

  RPCError commit_transaction(BlockID block, TransactionID tx,
                              NewContractBatch new_contracts);
  RPCError abort_transaction(BlockID block, TransactionID tx);
  RPCError try_push_new_subtransaction(BlockID block, TransactionID tx,
                                       StorageDeltaBatch const &batch);
  RPCError pop_subtransactions(BlockID block, TransactionID tx,
                               uint32_t popcount);
  ExpectedStorageItem get_snapshot_item(BlockID block,
                                        AddressAndKey const &addrkey);

  ExpectedContract get_contract(BlockID block, SHA256Hash const &hash);
  ExpectedDeployment get_deployment(BlockID block,
                                    ContractAddress const &contract_address);
  static ExpectedRetrocausalQueueBlock
  get_next_retrocausal_queue_block(BlockID block, AddressAndKey const &addrkey,
                                   uint64_t search_start_number);
  static ExpectedQueueEntry get_concurrent_retrocausal_queue_entry(
      BlockID block, AddressAndKey const &addrkey, uint32_t index);

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
                              StorageDelta const &delta,
                              StorageItemRPCFlagSet flags);
};

static_assert(
    is_DatabaseToExecutorInterfaceV1<HogDBExecutorRpc *, std::type_identity_t>);

class HogDBExecutorServer : public RpcServer<HogDBExecutorRpc> {
public:
  explicit HogDBExecutorServer(RPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
