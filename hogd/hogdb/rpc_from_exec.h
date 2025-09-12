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
class HogDBExecutorServer {
public:
  using rpc_service = DatabaseToExecutorInterfaceV1;

  explicit HogDBExecutorServer(RPCContext &ctx) : ctx_(ctx) {}

  // Start the server on the specified port
  void start_server(uint16_t port);

  RPCError commit_transaction(BlockID block, TransactionID tx);
  RPCError abort_transaction(BlockID block, TransactionID tx);
  RPCError try_push_new_subtransaction(BlockID block, TransactionID tx,
                                       StorageDeltaBatch const &batch);
  RPCError pop_subtransactions(BlockID block, TransactionID tx,
                               uint32_t popcount);
  ExpectedStorageItem get_snapshot_item(BlockID block,
                                        AddressAndKey const &addrkey);
  RPCError commit_contract_create(BlockID block, Contract const &contract);
  RPCError commit_contract_deploy(BlockID block, ContractAddress const &addr,
                                  SHA256Hash const &hash);
  ExpectedContract get_contract(BlockID block, SHA256Hash const &hash);
  ExpectedDeployment get_deployment(BlockID block,
                                    ContractAddress const &contract_address);
  static ExpectedRetrocausalQueueBlock
  get_next_retrocausal_queue_block(BlockID block, AddressAndKey const &addrkey,
                                   uint64_t search_start_number);
  static ExpectedQueueEntry get_concurrent_retrocausal_queue_entry(
      BlockID block, AddressAndKey const &addrkey, uint32_t index);

  uint16_t port() const { return port_number_; }

  ~HogDBExecutorServer()
  {
    listen_thread_.request_stop();
    shutdown(lfd_.get(), SHUT_RDWR);
    lfd_.reset();
  }

private:
  RPCContext &ctx_;

  uint16_t port_number_ = 0;
  std::jthread listen_thread_;
  x23::UniqueFd lfd_;

  static RPCError request_one(InvokeTX &tx, AddressAndKey const &addrkey,
                              StorageDelta const &delta,
                              StorageItemRPCFlagSet flags);
};

static_assert(is_DatabaseToExecutorInterfaceV1<HogDBExecutorServer *,
                                               std::type_identity_t>);

} // namespace hogd
