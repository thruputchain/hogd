#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <utility>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/block_state.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "hogd/rpc/rpc.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

class SyncDBExecutorServer {
public:
  explicit SyncDBExecutorServer(BlockManager *block_manager)
    : block_manager_(block_manager)
  {
    assert(block_manager_);
  }

  RPCError commit_transaction(BlockID block, TransactionID tx);
  RPCError abort_transaction(BlockID block, TransactionID tx);
  RPCError try_push_new_subtransaction(BlockID block,
                                      TransactionID tx,
                                      StorageDeltaBatch const& batch);
  RPCError pop_subtransactions(BlockID block, TransactionID tx,
                              uint32_t popcount);
  ExpectedStorageItem
  get_snapshot_item(BlockID block, AddressAndKey const &addrkey);
  RPCError commit_contract_create(BlockID block, Contract const& contract);
  RPCError commit_contract_deploy(BlockID block,
                                ContractAddress const &addr,
                                SHA256Hash const &hash);
  ExpectedContract get_contract(BlockID block,
                              SHA256Hash const &contract_hash);
  ExpectedDeployment
  get_deployment(BlockID block, ContractAddress const &contract_address);

  ExpectedRetrocausalQueueBlock
  get_next_retrocausal_queue_block(BlockID block, AddressAndKey const &addrkey,
                                  uint64_t search_start_number);

  ExpectedQueueEntry get_concurrent_retrocausal_queue_entry(
      BlockID block, AddressAndKey const &addrkey, uint32_t index);

private:

  static bool try_reserve_delta(BlockState *block_state, TransactionID tx,
                        AddressAndKey const &addrkey, StorageDelta const &delta,
                        StorageItemRPCFlagSet flags);
  BlockManager *block_manager_;
};

OwningSpan<const std::byte>
handle_adam_rpc_request(SyncDBExecutorServer& server, std::span<const std::byte> args);

class AdamRPCDbExecutorServer {
public:
  explicit AdamRPCDbExecutorServer(BlockManager *block_manager, uint32_t port)
    : sync_server_(block_manager)
  {
    auto res = RPCServer::create(port, [this](std::span<const std::byte> args) -> OwningSpan<const std::byte> {
      return handle_adam_rpc_request(sync_server_, args);
    });
    if (!res) {
      throw std::runtime_error("Failed to create RPC server");
    }
    rpc_server_ = std::move(*res);
    std::println("created rpc server");
    server_listener_ = std::jthread([this] {
      std::println("server listener thread starting");
      if (ErrnoStatus<void> ret = rpc_server_->listen(); !ret) {
        std::println("listen failed: {}", ret.error());
      }
      std::println("server listener thread exiting");
    });
  }

  ~AdamRPCDbExecutorServer() {
    std::println("stopping rpc server");
    rpc_server_->stop();
    std::println("server listener thread starting join");
    server_listener_.join();
    std::println("server listener thread joined");
  }

  uint32_t port() const { return rpc_server_->get_port(); }

private:
  SyncDBExecutorServer sync_server_;
  std::shared_ptr<RPCServer> rpc_server_;
  std::jthread server_listener_;
};

// Async server implementing the DatabaseToExecutorInterfaceV1 interface
// for handling database operations from the executor.
class DbExecutorServer {
public:
  using rpc_service = DatabaseToExecutorInterfaceV1;

  explicit DbExecutorServer(BlockManager *block_manager)
    : sync_server_(block_manager)
  {}

  // Start the server on the specified port
  sockaddr_storage start_server(uint32_t port_number);

  // RPC methods

  x23::Async<RPCError> commit_transaction(BlockID block, TransactionID tx);
  x23::Async<RPCError> abort_transaction(BlockID block, TransactionID tx);
  x23::Async<RPCError> try_push_new_subtransaction(BlockID block,
                                                   TransactionID tx,
                                                   StorageDeltaBatch batch);
  x23::Async<RPCError> pop_subtransactions(BlockID block, TransactionID tx,
                                           uint32_t popcount);
  x23::Async<ExpectedStorageItem>
  get_snapshot_item(BlockID block, AddressAndKey const &addrkey);
  x23::Async<RPCError> commit_contract_create(BlockID block, Contract const& contract);
  x23::Async<RPCError> commit_contract_deploy(BlockID block,
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
  SyncDBExecutorServer sync_server_;

  x23::ThreadPool pool_ = x23::ThreadPool(2);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);
};

static_assert(is_DatabaseToExecutorInterfaceV1<DbExecutorServer *, x23::Async>);

} // namespace hogd
