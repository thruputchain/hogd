#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <utility>

// generated x23 headers
#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/rpc/rpc.h"

namespace hogd {

template<typename Server>
requires is_DatabaseToExecutorInterfaceV1<Server *, std::type_identity_t>
OwningSpan<const std::byte>
hogd_from_execd_handler(Server &server, std::span<const std::byte> args,
                        std::string_view peer)
{
  auto header_result = rpc_server_parse_and_strip_header(args);
  if (!header_result) {
    return vec_as_bytes(response_header_to_buf(
        RPCResponseHeader{.error = RPCReponseErrorCode::kRpcInvalidArgument,
                          .xid = 0}));
  }
  auto [header, arg_span] = header_result.value();

  switch (header.method) {
  case get_rpc_method_number<DatabaseToExecutorInterfaceV1,
                             "try_push_new_subtransaction">():
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1,
                                      "try_push_new_subtransaction">(server,
                                                                     peer,
                                                                     header.xid,
                                                                     arg_span);
  case get_rpc_method_number<DatabaseToExecutorInterfaceV1,
                             "commit_transaction">():
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1,
                                      "commit_transaction">(server, peer,
                                                            header.xid,
                                                            arg_span);
  case get_rpc_method_number<DatabaseToExecutorInterfaceV1,
                             "abort_transaction">():
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1,
                                      "abort_transaction">(server, peer,
                                                           header.xid,
                                                           arg_span);
  case get_rpc_method_number<DatabaseToExecutorInterfaceV1,
                             "pop_subtransactions">():
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1,
                                      "pop_subtransactions">(server, peer,
                                                             header.xid,
                                                             arg_span);
  case get_rpc_method_number<DatabaseToExecutorInterfaceV1,
                             "get_snapshot_item">():
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1,
                                      "get_snapshot_item">(server, peer,
                                                           header.xid,
                                                           arg_span);
  case get_rpc_method_number<DatabaseToExecutorInterfaceV1,
                             "get_next_retrocausal_queue_block">():
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1,
                                      "get_next_retrocausal_queue_block">(
        server, peer, header.xid, arg_span);
  case get_rpc_method_number<DatabaseToExecutorInterfaceV1,
                             "get_concurrent_retrocausal_queue_entry">():
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1,
                                      "get_concurrent_retrocausal_queue_entry">(
        server, peer, header.xid, arg_span);
  case get_rpc_method_number<DatabaseToExecutorInterfaceV1,
                             "commit_contract_create">():
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1,
                                      "commit_contract_create">(server, peer,
                                                                header.xid,
                                                                arg_span);
  case get_rpc_method_number<DatabaseToExecutorInterfaceV1,
                             "commit_contract_deploy">():
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1,
                                      "commit_contract_deploy">(server, peer,
                                                                header.xid,
                                                                arg_span);
  case get_rpc_method_number<DatabaseToExecutorInterfaceV1, "get_contract">():
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1,
                                      "get_contract">(server, peer, header.xid,
                                                      arg_span);
  case get_rpc_method_number<DatabaseToExecutorInterfaceV1, "get_deployment">():
    return rpc_server_dispatch_method<DatabaseToExecutorInterfaceV1,
                                      "get_deployment">(server, peer,
                                                        header.xid, arg_span);
  default:
    return vec_as_bytes(response_header_to_buf(
        RPCResponseHeader{.error = RPCReponseErrorCode::kRpcUnknownMethod,
                          .xid = header.xid}));
  }
}

// Core logic handler for the db rpc server.
class HogDBExecutorServer {
public:
  explicit HogDBExecutorServer(RPCContext &ctx) : ctx_(ctx) {}

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

private:
  RPCContext &ctx_;

  static RPCError request_one(InvokeTX &tx, AddressAndKey const &addrkey,
                              StorageDelta const &delta,
                              StorageItemRPCFlagSet flags);
};

static_assert(is_DatabaseToExecutorInterfaceV1<HogDBExecutorServer *,
                                               std::type_identity_t>);

// Wraps the HogDBExecutorServer into an RPCServer.
class HogDBExecutorRPCServer {
public:
  HogDBExecutorRPCServer(RPCContext &ctx, uint16_t port) : exec_server_(ctx)
  {
    auto res = RPCServer::create(
        port,
        [this](std::span<const std::byte> args,
               std::string_view peer) -> OwningSpan<const std::byte> {
          return hogd_from_execd_handler(exec_server_, args, peer);
        });
    if (!res) {
      throw std::runtime_error("Failed to create RPC server");
    }
    rpc_server_ = std::move(*res);
    rpc_listener_th_ = std::jthread([this] {
      if (ErrnoStatus<void> ret = rpc_server_->listen(); !ret) {
        error("Database to executor server listen failed: {}", ret.error());
      }
    });
  }

  ~HogDBExecutorRPCServer() { rpc_server_->stop(); }

  uint16_t port() const { return rpc_server_->get_port(); }

private:
  HogDBExecutorServer exec_server_;
  std::shared_ptr<RPCServer> rpc_server_;
  std::jthread rpc_listener_th_;
};

} // namespace hogd
