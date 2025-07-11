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
#include "x23/async.h"
#include "x23/event.h"
#include "x23/serialize.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {

// Template to check if a method is in the valid_procnos array
template<uint32_t Method, size_t Index = 0>
constexpr bool
is_valid_method()
{
  if constexpr (Index >= DatabaseToExecutorInterfaceV1::valid_procnos.size()) {
    return false;
  }
  else if constexpr (DatabaseToExecutorInterfaceV1::valid_procnos[Index] ==
                     Method) {
    return true;
  }
  else {
    return is_valid_method<Method, Index + 1>();
  }
}

template<typename Server, uint32_t Method>
OwningSpan<const std::byte>
dispatch_request(Server &server, std::span<const std::byte> args)
{
  static_assert(is_valid_method<Method>(), "method is not valid");
  using meta = DatabaseToExecutorInterfaceV1::proc_meta<Method>;
  typename meta::arg_type a;
  // TODO(geoff): fix the type conversion
  x23::deserialize(&a,
                   std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(
                                                args.data()),
                                            args.size()));
  auto res = meta::dispatch(server, a);
  return vec_as_bytes(x23::serialize(res));
}

template<typename Server>
requires is_DatabaseToExecutorInterfaceV1<Server *, std::type_identity_t>
OwningSpan<const std::byte>
handle_db_executor_rpc_request(Server &server, std::span<const std::byte> args)
{
  uint32_t method;
  if (args.size() < sizeof(method)) {
    return OwningSpan<const std::byte>();
  }
  std::memcpy(&method, args.data(), sizeof(method));

  // TODO(geoff): figure out a convenient way to automate this
  // static constexpr std::array<std::uint32_t, 11> valid_procnos{
  //    0, 1, 2, 3, 100, 101, 102, 200, 201, 202, 203
  //};
  try {
    switch (method) {
    case 0:
      return dispatch_request<Server, 0>(server, args.subspan(sizeof(method)));
    case 1:
      return dispatch_request<Server, 1>(server, args.subspan(sizeof(method)));
    case 2:
      return dispatch_request<Server, 2>(server, args.subspan(sizeof(method)));
    case 3:
      return dispatch_request<Server, 3>(server, args.subspan(sizeof(method)));
    case 100:
      return dispatch_request<Server, 100>(server,
                                           args.subspan(sizeof(method)));
    case 101:
      return dispatch_request<Server, 101>(server,
                                           args.subspan(sizeof(method)));
    case 102:
      return dispatch_request<Server, 102>(server,
                                           args.subspan(sizeof(method)));
    case 200:
      return dispatch_request<Server, 200>(server,
                                           args.subspan(sizeof(method)));
    case 201:
      return dispatch_request<Server, 201>(server,
                                           args.subspan(sizeof(method)));
    case 202:
      return dispatch_request<Server, 202>(server,
                                           args.subspan(sizeof(method)));
    case 203:
      return dispatch_request<Server, 203>(server,
                                           args.subspan(sizeof(method)));
    default:
      return OwningSpan<const std::byte>();
    }
  } catch (const x23::XdrException &e) {
    return OwningSpan<const std::byte>();
  }
}

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

class AdamRPCDbExecutorServer {
public:
  explicit AdamRPCDbExecutorServer(BlockManager *block_manager, uint32_t port)
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

  ~AdamRPCDbExecutorServer() { rpc_server_->stop(); }

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
  x23::Async<RPCError> commit_contract_create(BlockID block,
                                              Contract const &contract);
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
