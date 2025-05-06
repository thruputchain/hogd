#include "hogd/hogexec/runtime/database_proxy.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
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

template<typename T, auto NullValue>
auto
to_host_fn_error_code(auto err, auto &&f)
{
  using ret_t = decltype(f(std::move(get<1>(err))));
  switch (err.error()) {
  case RPCError::kRPCNone:
    return HostFnStatus<ret_t>(f(std::move(get<1>(err))));
  case RPCError::kRPCNoStorageItem:
  case RPCError::kRPCNoContractDeployed:
  case RPCError::kRPCNoQueueEntry:
  case RPCError::kRPCStorageWriteFailed:
    return HostFnStatus<ret_t>(NullValue);
  case RPCError::kRPCBadBlockID:
    return HostFnStatus<ret_t>(
        HostFnError{HostFnErrorCode::kHostAbortTransaction});
  case RPCError::kRPCValidationFailure:
    return HostFnStatus<ret_t>(
        HostFnError{HostFnErrorCode::kHostValidationFailure});
  default:
    return HostFnStatus<ret_t>(
        HostFnError{HostFnErrorCode::kHostUnrecoverable});
  }
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

x23::Async<HostFnStatus<std::unique_ptr<const StorageItem>>>
DatabaseProxy::get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey)
{
  auto res = co_await read_client_.get_snapshot_item(block_id, addrkey);
  co_return to_host_fn_error_code<StorageItem, nullptr>(
      res, [](StorageItem &&x) {
        return std::make_unique<StorageItem>(std::move(x));
      });
}

x23::Async<HostFnStatus<std::shared_ptr<const Contract>>>
DatabaseProxy::get_contract(BlockID block_id, SHA256Hash const &contract_hash)
{
  auto res = co_await read_client_.get_contract(block_id, contract_hash);
  co_return to_host_fn_error_code<const Contract, nullptr>(
      res,
      [](const Contract &x) { return std::make_shared<const Contract>(x); });
}

x23::Async<HostFnStatus<std::optional<SHA256Hash>>>
DatabaseProxy::get_deployment(BlockID block_id, ContractAddress const &addr)
{
  auto res = co_await read_client_.get_deployment(block_id, addr);
  co_return to_host_fn_error_code<SHA256Hash, std::nullopt>(
      res, [](SHA256Hash &&x) {
        return std::make_optional<SHA256Hash>(std::move(x));
      });
}

x23::Async<HostFnStatus<std::optional<RetrocausalQueueBlock>>>
DatabaseProxy::get_next_retrocausal_queue_block(BlockID block_id,
                                                AddressAndKey const &addrkey,
                                                uint64_t search_start_number)
{
  auto res = co_await read_client_.get_next_retrocausal_queue_block(
      block_id, addrkey, search_start_number);
  co_return to_host_fn_error_code<RetrocausalQueueBlock, std::nullopt>(
      res, [](RetrocausalQueueBlock &&x) {
        return std::make_optional<RetrocausalQueueBlock>(std::move(x));
      });
}

x23::Async<HostFnStatus<std::optional<QueueEntry>>>
DatabaseProxy::get_concurrent_retrocausal_queue_entry(
    BlockID block_id, AddressAndKey const &addrkey, uint32_t index)
{
  auto res =
      co_await read_client_.get_concurrent_retrocausal_queue_entry(block_id,
                                                                   addrkey,
                                                                   index);
  co_return to_host_fn_error_code<QueueEntry, std::nullopt>(
      res, [](QueueEntry &&x) {
        return std::make_optional<QueueEntry>(std::move(x));
      });
}

x23::Async<HostFnStatus<std::optional<int128_t>>>
DatabaseProxy::try_reserve_delta(BlockID block_id, TransactionID tx_id,
                                 AddressAndKey const &addrkey,
                                 StorageDelta const &delta,
                                 StorageItemRPCFlagSet flags)
{
  auto res = co_await read_client_.try_reserve_delta(block_id, tx_id, addrkey,
                                                     delta, flags);
  co_return to_host_fn_error_code<int128_xdr, std::nullopt>(
      res, [](int128_xdr &&x) {
        return std::make_optional<int128_t>(from_xdr(std::move(x)));
      });
}

x23::Async<void>
DatabaseProxy::commit_transaction(BlockID block_id, TransactionID tx_id)
{
  co_await read_client_.commit_transaction(block_id, tx_id);
}

x23::Async<void>
DatabaseProxy::abort_transaction(BlockID block_id, TransactionID tx_id)
{
  co_await read_client_.abort_transaction(block_id, tx_id);
}

} // namespace hogd
