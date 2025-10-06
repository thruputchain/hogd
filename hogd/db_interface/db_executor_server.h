#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <utility>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/block_state.h"
#include "rpc_types.hh"
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

// Core logic handler for the db rpc server.
class DbExecutorRpc {
public:
  using rpc_service = DatabaseToExecutorInterfaceV1;

  explicit DbExecutorRpc(BlockManager *block_manager)
    : block_manager_(block_manager)
  {
    assert(block_manager_);
  }

  RPCError commit_transaction(BlockID block, TransactionID tx,
                              NewContractBatch new_contracts);
  RPCError abort_transaction(BlockID block, TransactionID tx);
  // Increases subtransaction count by 1 IF SUCCESSFUL.
  // Otherwise does not change subtransaction count, and is a no-op.
  RPCError try_push_new_subtransaction(BlockID block, TransactionID tx,
                                       StorageDeltaBatch const &batch);
  RPCError pop_subtransactions(BlockID block, TransactionID tx,
                               uint32_t popcount);
  ExpectedStorageItem get_snapshot_item(BlockID block,
                                        AddressAndKey const &addrkey);
  RPCError commit_contract_create(BlockID block, Contract const &contract);

  ExpectedContract get_contract(BlockID block, SHA256Hash const &contract_hash);
  ExpectedDeployment get_deployment(BlockID block,
                                    ContractAddress const &contract_address);

  ExpectedRetrocausalQueueBlock
  get_next_retrocausal_queue_block(BlockID block, AddressAndKey const &addrkey,
                                   uint64_t search_start_number);

  ExpectedQueueEntry get_concurrent_retrocausal_queue_entry(
      BlockID block, AddressAndKey const &addrkey, uint32_t index);

private:
  static bool try_reserve_delta(BlockState *block_state, TransactionID tx,
                                AddressAndKey const &addrkey,
                                StorageDelta const &delta,
                                StorageItemRPCFlagSet flags);
  BlockManager *block_manager_;
};

static_assert(
    is_DatabaseToExecutorInterfaceV1<DbExecutorRpc *, std::type_identity_t>);

class DbExecutorServer : public RpcServer<DbExecutorRpc> {
public:
  DbExecutorServer(BlockManager *block_manager) : RpcServer(block_manager) {}
};

} // namespace hogd
