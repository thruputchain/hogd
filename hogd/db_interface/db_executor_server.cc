#include "hogd/db_interface/db_executor_server.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <sys/socket.h>
#include <utility>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/db_interface/block_state.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

namespace {
template<uint32_t Method> 
OwningSpan<const std::byte> dispatch_request(SyncDBExecutorServer& server, std::span<const std::byte> args)
{
  using meta = DatabaseToExecutorInterfaceV1::proc_meta<Method>;
  typename meta::arg_type a;
  // TODO(geoff): fix the type conversion
  x23::deserialize(&a, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(args.data()), args.size()));
  auto res = meta::dispatch(server, a);
  return vec_as_bytes(x23::serialize(res));
}
}

OwningSpan<const std::byte>
handle_adam_rpc_request(SyncDBExecutorServer& server, std::span<const std::byte> args)
{
  uint32_t method;
  if (args.size() < sizeof(method)) {
    std::println("args too small: {}", args.size());
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
      return dispatch_request<0>(server, args.subspan(sizeof(method)));
    case 1:
      return dispatch_request<1>(server, args.subspan(sizeof(method)));
    case 2:
      return dispatch_request<2>(server, args.subspan(sizeof(method)));
    case 3:
      return dispatch_request<3>(server, args.subspan(sizeof(method)));
    case 100:
      return dispatch_request<100>(server, args.subspan(sizeof(method)));
    case 101:
      return dispatch_request<101>(server, args.subspan(sizeof(method)));
    case 102:
      return dispatch_request<102>(server, args.subspan(sizeof(method)));
    case 200:
      return dispatch_request<200>(server, args.subspan(sizeof(method)));
    case 201:
      return dispatch_request<201>(server, args.subspan(sizeof(method)));
    case 202:
      return dispatch_request<202>(server, args.subspan(sizeof(method)));
    case 203:
      return dispatch_request<203>(server, args.subspan(sizeof(method)));
    default:
      std::println("unknown method: {}", method);
      return OwningSpan<const std::byte>();
  }
  } catch (const x23::XdrException &e) {
    std::println("xdr exception in server: {}", e.what());
    return OwningSpan<const std::byte>();
  }
}

// SyncDBExecutorServer implementations

bool
SyncDBExecutorServer::try_reserve_delta(BlockState *block_state, TransactionID tx,
                                        AddressAndKey const &addrkey,
                                        StorageDelta const &delta,
                                        StorageItemRPCFlagSet flags)
{
  return block_state->try_reserve_storage_delta(tx, addrkey, delta, flags);
}

RPCError
SyncDBExecutorServer::commit_transaction(BlockID block, TransactionID tx)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return RPCError::kRPCBadBlockID;
  }

  block_data->commit_transaction(tx);
  return RPCError::kRPCNone;
}

RPCError
SyncDBExecutorServer::abort_transaction(BlockID block, TransactionID tx)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return RPCError::kRPCBadBlockID;
  }

  block_data->abort_transaction(tx);
  return RPCError::kRPCNone;
}

RPCError
SyncDBExecutorServer::try_push_new_subtransaction(BlockID block, TransactionID tx,
                                                  StorageDeltaBatch const& batch)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return RPCError::kRPCBadBlockID;
  }

  block_data->push_subtransaction(tx);

  for (auto const &delta : batch) {
    if (!try_reserve_delta(block_data.get(), tx, delta.addrkey, delta.delta,
                           delta.flags)) {
      block_data->pop_subtransactions(tx, 1);
      return RPCError::kRPCStorageWriteFailed;
    }
  }

  return RPCError::kRPCNone;
}

RPCError
SyncDBExecutorServer::pop_subtransactions(BlockID block, TransactionID tx,
                                          uint32_t popcount)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return RPCError::kRPCBadBlockID;
  }

  block_data->pop_subtransactions(tx, popcount);
  return RPCError::kRPCNone;
}

ExpectedStorageItem
SyncDBExecutorServer::get_snapshot_item(BlockID block, AddressAndKey const &addrkey)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return ExpectedStorageItem(RPCError::kRPCBadBlockID);
  }

  auto const *item =
      block_data->get_snapshot_view()->get_snapshot_storage_item(addrkey);
  if (!item) {
    return ExpectedStorageItem(RPCError::kRPCNoStorageItem);
  }

  ExpectedStorageItem out;
  out.error(RPCError::kRPCNone);
  out.item() = item->to_wire_item();
  return out;
}

RPCError
SyncDBExecutorServer::commit_contract_create(BlockID block, Contract const& contract)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return RPCError::kRPCBadBlockID;
  }

  block_data->commit_contract_create(contract);
  return RPCError::kRPCNone;
}

RPCError
SyncDBExecutorServer::commit_contract_deploy(BlockID block,
                                             ContractAddress const &addr,
                                             SHA256Hash const &hash)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return RPCError::kRPCBadBlockID;
  }

  block_data->commit_contract_deploy(addr, hash);
  return RPCError::kRPCNone;
}

ExpectedContract
SyncDBExecutorServer::get_contract(BlockID block, SHA256Hash const &contract_hash)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return ExpectedContract(RPCError::kRPCBadBlockID);
  }

  auto contract =
      block_data->get_snapshot_view()->get_snapshot_contract(contract_hash);
  if (!contract) {
    return ExpectedContract(RPCError::kRPCContractNotExist);
  }

  ExpectedContract out;
  out.error(RPCError::kRPCNone);
  out.contract() = *contract;
  return out;
}

ExpectedDeployment
SyncDBExecutorServer::get_deployment(BlockID block,
                                     ContractAddress const &contract_address)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return ExpectedDeployment(RPCError::kRPCBadBlockID);
  }

  auto hash = block_data->get_snapshot_view()->get_snapshot_deployment(
      contract_address);
  if (!hash) {
    return ExpectedDeployment(RPCError::kRPCNoContractDeployed);
  }

  ExpectedDeployment out;
  out.error(RPCError::kRPCNone);
  out.deployed_hash() = *hash;
  return out;
}

ExpectedRetrocausalQueueBlock
SyncDBExecutorServer::get_next_retrocausal_queue_block(BlockID block,
                                                       AddressAndKey const &addrkey,
                                                       uint64_t search_start_number)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCBadBlockID);
  }

  auto const *item =
      block_data->get_snapshot_view()->get_snapshot_storage_item(addrkey);
  if (!item) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNoStorageItem);
  }

  if (item->type() != StorageItemType::kItemTypeRetrocausalQueue) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNoStorageItem);
  }

  auto const &queue = item->queue();
  const auto *next_block = queue.try_get_next_block(search_start_number);
  if (!next_block) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNoQueueEntry);
  }

  ExpectedRetrocausalQueueBlock out;
  out.error(RPCError::kRPCNone);
  out.queue_block() = *next_block;
  return out;
}

ExpectedQueueEntry
SyncDBExecutorServer::get_concurrent_retrocausal_queue_entry(
    BlockID block, AddressAndKey const &addrkey, uint32_t index)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return ExpectedQueueEntry(RPCError::kRPCBadBlockID);
  }

  auto entry =
      block_data->get_concurrent_retrocausal_queue_entry(addrkey, index);
  if (!entry.get()) {
    return ExpectedQueueEntry(RPCError::kRPCNoQueueEntry);
  }

  ExpectedQueueEntry out;
  out.error(RPCError::kRPCNone);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  out.queue_entry() = *entry.get();
  return out;
}

sockaddr_storage
DbExecutorServer::start_server(uint32_t port_number)
{
  auto lfd = x23::tcplisten(port_number);
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  auto out = x23::local_addr(lfd);

  [](x23::CoroPoll &cp, DbExecutorServer *server,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await cp.pool_.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c).server(server);
    }
  }(cp_, this, std::move(lfd));

  return out;
}

x23::Async<RPCError>
DbExecutorServer::commit_transaction(BlockID block, TransactionID tx)
{
  co_return sync_server_.commit_transaction(block, tx);
}

x23::Async<RPCError>
DbExecutorServer::abort_transaction(BlockID block, TransactionID tx)
{
  co_return sync_server_.abort_transaction(block, tx);
}

x23::Async<RPCError>
DbExecutorServer::try_push_new_subtransaction(BlockID block, TransactionID tx,
                                              StorageDeltaBatch batch)
{
  co_return sync_server_.try_push_new_subtransaction(block, tx, batch);
}

x23::Async<RPCError>
DbExecutorServer::pop_subtransactions(BlockID block, TransactionID tx,
                                      uint32_t popcount)
{
  co_return sync_server_.pop_subtransactions(block, tx, popcount);
}

x23::Async<ExpectedStorageItem>
DbExecutorServer::get_snapshot_item(BlockID block, AddressAndKey const &addrkey)
{
  co_return sync_server_.get_snapshot_item(block, addrkey);
}

x23::Async<RPCError>
DbExecutorServer::commit_contract_create(BlockID block, Contract const& contract)
{
  co_return sync_server_.commit_contract_create(block, contract);
}

x23::Async<RPCError>
DbExecutorServer::commit_contract_deploy(BlockID block,
                                         ContractAddress const &addr,
                                         SHA256Hash const &hash)
{
  co_return sync_server_.commit_contract_deploy(block, addr, hash);
}

x23::Async<ExpectedContract>
DbExecutorServer::get_contract(BlockID block, SHA256Hash const &contract_hash)
{
  co_return sync_server_.get_contract(block, contract_hash);
}

x23::Async<ExpectedDeployment>
DbExecutorServer::get_deployment(BlockID block,
                                 ContractAddress const &contract_address)
{
  co_return sync_server_.get_deployment(block, contract_address);
}

x23::Async<ExpectedRetrocausalQueueBlock>
DbExecutorServer::get_next_retrocausal_queue_block(BlockID block,
                                                   AddressAndKey const &addrkey,
                                                   uint64_t search_start_number)
{
  co_return sync_server_.get_next_retrocausal_queue_block(block, addrkey, search_start_number);
}

x23::Async<ExpectedQueueEntry>
DbExecutorServer::get_concurrent_retrocausal_queue_entry(
    BlockID block, AddressAndKey const &addrkey, uint32_t index)
{
  co_return sync_server_.get_concurrent_retrocausal_queue_entry(block, addrkey, index);
}

} // namespace hogd
