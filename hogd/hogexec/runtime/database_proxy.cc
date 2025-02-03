#include "hogd/hogexec/runtime/database_proxy.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "contract.hh"
#include "coord.hh"
#include "rpc.hh"
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
connect_to_database(x23::CoroPoll &cp, DatabaseInfo const &db_info)
{
  auto fd = cp.tcpconnect(std::string(db_info.database_address),
                          std::string(db_info.database_to_exec_port))();
  if (!fd) {
    return std::nullopt;
  }
  auto client_config = x23::rpc_stream_logged(cp, *fd);
  return client_config.template client<DatabaseToExecutorInterfaceV1>();
}

} // namespace

DatabaseProxy::DatabaseProxy(x23::CoroPoll &cp, DatabaseInfo const &db_info)
  : db_client_([&] {
      auto db_client = connect_to_database(cp, db_info);

      while (!db_client) {
        perror(std::format("connect to database exec server {} port {} failed, "
                           "retrying...",
                           std::string(db_info.database_address),
                           std::string(db_info.database_to_exec_port))
                   .c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        db_client = connect_to_database(cp, db_info);
      }
      return *db_client;
    }())
{}

x23::Async<bool>
DatabaseProxy::try_reserve_delta(BlockID block_id, TransactionID tx_id,
                                 AddressAndKey const &addrkey,
                                 FlaggedStorageDelta const &delta,
                                 RPCFlagSet flags)
{
  if (delta.flags & std::to_underlying(StorageDeltaModifier::kLockExclusive)) {
    flags |= std::to_underlying(RPCFlags::kExclusiveLock);
  }
  auto res = co_await db_client_.try_reserve_delta(block_id, tx_id, addrkey,
                                                   delta.delta, flags);

  if (res == RPCError::kNone) {
    co_return true;
  }
  if (res == RPCError::kStorageWriteFailed) {
    co_return false;
  }
  // This function could also return an expected, but somehow
  // this needs to get translated to a kUnrecoverable error.
  throw std::runtime_error("unhandleable error");
}

x23::Async<void>
DatabaseProxy::commit_transaction(BlockID block_id, TransactionID tx_id)
{
  co_await db_client_.commit_transaction(block_id, tx_id);
  co_return;
}

x23::Async<void>
DatabaseProxy::abort_transaction(BlockID block_id, TransactionID tx_id)
{
  return db_client_.abort_transaction(block_id, tx_id);
}

std::optional<StorageItem>
DatabaseProxy::get_item(BlockID block_id, AddressAndKey const &addrkey)
{
  auto res = db_client_.get_item(block_id, addrkey)();

  if (res.error() == RPCError::kNone) {
    return res.item();
  }
  if (res.error() == RPCError::kNoStorageItem) {
    return std::nullopt;
  }
  throw std::runtime_error("unhandleable error");
}

std::optional<RetrocausalQueueBlock>
DatabaseProxy::get_retrocausal_queue_block(BlockID block_id,
                                           AddressAndKey const &addrkey,
                                           uint64_t index)
{
  auto res =
      db_client_.get_next_retrocausal_queue_block(block_id, addrkey, index)();
  if (res.error() == RPCError::kNone) {
    return res.block();
  }
  if (res.error() == RPCError::kNoRetrocausalQueueItem ||
      res.error() == RPCError::kNoStorageItem) {
    return std::nullopt;
  }
  throw std::runtime_error("unhandleable error");
}
// WARNING: nondeterministic
std::optional<QueueEntry>
DatabaseProxy::get_concurrent_retrocausal_queue_entry(
    BlockID block_id, AddressAndKey const &addrkey, uint64_t index)
{
  auto res =
      db_client_.get_concurrent_retrocausal_queue_entry(block_id, addrkey,
                                                        index)();
  if (res.error() == RPCError::kNone) {
    return res.entry();
  }
  if (res.error() == RPCError::kNoRetrocausalQueueItem) {
    return std::nullopt;
  }
  throw std::runtime_error("unhandleable error");
}

x23::Async<void>
DatabaseProxy::commit_contract_create(BlockID block_id,
                                      Contract const &contract)
{
  return db_client_.commit_contract_create(block_id, contract);
}

x23::Async<void>
DatabaseProxy::commit_contract_deploy(BlockID block_id,
                                      ContractAddress const &addr,
                                      SHA256Hash const &contract_hash)
{
  return db_client_.commit_contract_deploy(block_id, addr, contract_hash);
}

std::shared_ptr<const Contract>
DatabaseProxy::get_contract(BlockID block_id, SHA256Hash const &contract_hash)
{
  auto res = db_client_.get_contract(block_id, contract_hash)();

  if (res.error() == RPCError::kNone) {
    return std::make_shared<const Contract>(res.contract());
  }
  if (res.error() == RPCError::kContractNExist) {
    return nullptr;
  }
  throw std::runtime_error("unhandleable error");
}

std::optional<SHA256Hash>
DatabaseProxy::get_deployment(BlockID block_id, ContractAddress const &addr)
{
  auto res = db_client_.get_deployment(block_id, addr)();

  if (res.error() == RPCError::kNone) {
    return res.deployed_hash();
  }
  if (res.error() == RPCError::kNoContractDeployed) {
    return std::nullopt;
  }
  throw std::runtime_error("unhandleable error");
}

} // namespace hogd
