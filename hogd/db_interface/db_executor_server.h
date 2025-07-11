#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <utility>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/block_state.h"
#include "hogd/rpc/rpc.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

template<typename Server>
requires is_DatabaseToExecutorInterfaceV1<Server *, std::type_identity_t>
OwningSpan<const std::byte>
handle_db_executor_rpc_request(Server &server, std::span<const std::byte> args)
{
  auto header_result = rpc_server_parse_and_strip_header(args);
  if (!header_result) {
    return vec_as_bytes(response_header_to_buf(
        RPCResponseHeader{RPCReponseErrorCode::kRpcInvalidArgument}));
  }
  auto [method, arg_span] = header_result.value();

  // TODO(geoff): figure out a convenient way to automate this
  // static constexpr std::array<std::uint32_t, 11> valid_procnos{
  //    0, 1, 2, 3, 100, 101, 102, 200, 201, 202, 203
  //};
  switch (method) {
  case 0:
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1, 0,
                                      "try_push_new_subtransaction">(server,
                                                                     arg_span);
  case 1:
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1, 1,
                                      "commit_transaction">(server, arg_span);
  case 2:
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1, 2,
                                      "abort_transaction">(server, arg_span);
  case 3:
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1, 3,
                                      "pop_subtransactions">(server, arg_span);
  case 100:
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1, 100,
                                      "get_snapshot_item">(server, arg_span);
  case 101:
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1, 101,
                                      "get_next_retrocausal_queue_block">(
        server, arg_span);
  case 102:
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1, 102,
                                      "get_concurrent_retrocausal_queue_entry">(
        server, arg_span);
  case 200:
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1, 200,
                                      "commit_contract_create">(server,
                                                                arg_span);
  case 201:
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1, 201,
                                      "commit_contract_deploy">(server,
                                                                arg_span);
  case 202:
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1, 202,
                                      "get_contract">(server, arg_span);
  case 203:
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1, 203,
                                      "get_deployment">(server, arg_span);
  default:
    return vec_as_bytes(response_header_to_buf(
        RPCResponseHeader{RPCReponseErrorCode::kRpcUnknownMethod}));
  }
}

// Core logic handler for the db rpc server.
class SyncDBExecutorServer {
public:
  explicit SyncDBExecutorServer(BlockManager *block_manager)
    : block_manager_(block_manager)
  {
    assert(block_manager_);
  }

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

static_assert(is_DatabaseToExecutorInterfaceV1<SyncDBExecutorServer *,
                                               std::type_identity_t>);

// Wraps the SyncDBExecutorServer in an RPC server.
class RPCDbExecutorServer {
public:
  explicit RPCDbExecutorServer(BlockManager *block_manager, uint32_t port)
    : sync_server_(block_manager)
  {
    auto res = RPCServer::create(
        port,
        [this](std::span<const std::byte> args) -> OwningSpan<const std::byte> {
          return handle_db_executor_rpc_request(sync_server_, args);
        });
    if (!res) {
      throw std::runtime_error("Failed to create RPC server");
    }
    rpc_server_ = std::move(*res);
    server_listener_ = std::jthread([this] {
      if (ErrnoStatus<void> ret = rpc_server_->listen(); !ret) {
        error("Database to executor server listen failed: {}", ret.error());
      }
    });
  }

  ~RPCDbExecutorServer() { rpc_server_->stop(); }

  uint32_t port() const { return rpc_server_->get_port(); }

private:
  SyncDBExecutorServer sync_server_;
  std::shared_ptr<RPCServer> rpc_server_;
  std::jthread server_listener_;
};

} // namespace hogd
