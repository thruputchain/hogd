#include "hogd/hogexec/runtime/database_proxy.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"

namespace hogd {

namespace {

std::optional<DatabaseToExecutorInterfaceV1::client<
    x23::RpcStream<x23::RpcLogger>::ClientRef>>
connect_to_local_mock_database(x23::CoroPoll &cp, LocalMockDB local_db)
{
  auto fd =
      cp.tcpconnect("localhost", std::to_string(local_db.read_port_number_))();
  if (!fd) {
    return std::nullopt;
  }
  auto client_config = x23::rpc_stream_logged(cp, *fd);
  return client_config.template client<DatabaseToExecutorInterfaceV1>();
}

} // namespace

DatabaseProxy::DatabaseProxy(x23::CoroPoll &cp,
                             DatabaseIdentifier db_identifier)
  : read_client_([&] {
      if (std::holds_alternative<LocalMockDB>(db_identifier)) {
        auto mock_db = std::get<LocalMockDB>(db_identifier);

        auto out = connect_to_local_mock_database(cp, mock_db);

        while (!out) {
          perror(std::format("connect to local mock db read server localhost "
                             "port {} failed, "
                             "retrying...",
                             mock_db.read_port_number_)
                     .c_str());
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          out = connect_to_local_mock_database(cp, mock_db);
        }
        return *out;
      }
      // unknown database specification type
      std::terminate();
    }())
{}

x23::Async<std::unique_ptr<const StorageItem>>
DatabaseProxy::get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey)
{
  auto res = read_client_.get_snapshot_item(block_id, addrkey)();

  if (res.error() == RPCError::kRPCNone) {
    co_return std::make_unique<const StorageItem>(res.item());
  }
  if (res.error() == RPCError::kRPCNoStorageItem) {
    co_return nullptr;
  }
  throw std::runtime_error("unhandleable error");
}

x23::Async<std::shared_ptr<const Contract>>
DatabaseProxy::get_contract(BlockID block_id, SHA256Hash const &contract_hash)
{
  auto res = read_client_.get_contract(block_id, contract_hash)();

  if (res.error() == RPCError::kRPCNone) {
    co_return std::make_shared<const Contract>(res.contract());
  }
  if (res.error() == RPCError::kRPCContractNotExist) {
    co_return nullptr;
  }
  throw std::runtime_error("unhandleable error");
}

x23::Async<std::optional<SHA256Hash>>
DatabaseProxy::get_deployment(BlockID block_id, ContractAddress const &addr)
{
  auto res = read_client_.get_deployment(block_id, addr)();

  if (res.error() == RPCError::kRPCNone) {
    co_return res.deployed_hash();
  }
  if (res.error() == RPCError::kRPCNoContractDeployed) {
    co_return std::nullopt;
  }
  throw std::runtime_error("unhandleable error");
}

} // namespace hogd
