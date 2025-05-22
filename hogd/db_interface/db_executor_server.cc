#include "hogd/db_interface/db_executor_server.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

void
DbExecutorServer::start_server(uint32_t port_number)
{
  auto lfd = x23::tcplisten(port_number);
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  [](x23::CoroPoll &cp, DbExecutorServer *server,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await cp.pool_.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c).server(server);
    }
  }(cp_, this, std::move(lfd));
}

x23::Async<ExpectedStorageCost>
DbExecutorServer::try_reserve_delta(BlockID block, TransactionID tx,
                                    AddressAndKey addrkey, StorageDelta delta,
                                    StorageItemRPCFlagSet flags)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return ExpectedStorageCost(RPCError::kRPCBadBlockID);
  }

  auto reserve_res =
      block_data->try_reserve_storage_delta(tx, addrkey, delta, flags);

  if (!reserve_res) {
    co_return ExpectedStorageCost(RPCError::kRPCStorageWriteFailed);
  }

  ExpectedStorageCost out;
  out.error(RPCError::kRPCNone);
  out.storage_cost() = to_xdr(reserve_res.value());
  co_return out;
}

x23::Async<RPCError>
DbExecutorServer::commit_transaction(BlockID block, TransactionID tx)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return RPCError::kRPCBadBlockID;
  }

  block_data->commit_transaction(tx);
  co_return RPCError::kRPCNone;
}

x23::Async<RPCError>
DbExecutorServer::abort_transaction(BlockID block, TransactionID tx)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return RPCError::kRPCBadBlockID;
  }

  block_data->abort_transaction(tx);
  co_return RPCError::kRPCNone;
}

x23::Async<RPCError>
DbExecutorServer::push_new_subtransaction(BlockID block, TransactionID tx)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return RPCError::kRPCBadBlockID;
  }

  block_data->push_subtransaction(tx);
  co_return RPCError::kRPCNone;
}

x23::Async<RPCError>
DbExecutorServer::pop_subtransactions(BlockID block, TransactionID tx,
                                      uint32_t popcount)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return RPCError::kRPCBadBlockID;
  }

  block_data->pop_subtransactions(tx, popcount);
  co_return RPCError::kRPCNone;
}

x23::Async<ExpectedStorageItem>
DbExecutorServer::get_snapshot_item(BlockID block, AddressAndKey const &addrkey)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return ExpectedStorageItem(RPCError::kRPCBadBlockID);
  }

  auto const *item =
      block_data->get_snapshot_view()->get_snapshot_storage_item(addrkey);
  if (!item) {
    co_return ExpectedStorageItem(RPCError::kRPCNoStorageItem);
  }

  ExpectedStorageItem out;
  out.error(RPCError::kRPCNone);
  out.item() = item->to_wire_item();
  co_return out;
}

x23::Async<RPCError>
DbExecutorServer::commit_contract_create(BlockID block, Contract contract)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return RPCError::kRPCBadBlockID;
  }

  block_data->commit_contract_create(contract);
  co_return RPCError::kRPCNone;
}

x23::Async<RPCError>
DbExecutorServer::commit_contract_deploy(BlockID block,
                                         ContractAddress const &addr,
                                         SHA256Hash const &hash)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return RPCError::kRPCBadBlockID;
  }

  block_data->commit_contract_deploy(addr, hash);
  co_return RPCError::kRPCNone;
}

x23::Async<ExpectedContract>
DbExecutorServer::get_contract(BlockID block, SHA256Hash const &contract_hash)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return ExpectedContract(RPCError::kRPCBadBlockID);
  }

  auto contract =
      block_data->get_snapshot_view()->get_snapshot_contract(contract_hash);
  if (!contract) {
    co_return ExpectedContract(RPCError::kRPCContractNotExist);
  }

  ExpectedContract out;
  out.error(RPCError::kRPCNone);
  out.contract() = *contract;
  co_return out;
}

x23::Async<ExpectedDeployment>
DbExecutorServer::get_deployment(BlockID block,
                                 ContractAddress const &contract_address)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return ExpectedDeployment(RPCError::kRPCBadBlockID);
  }

  auto hash = block_data->get_snapshot_view()->get_snapshot_deployment(
      contract_address);
  if (!hash) {
    co_return ExpectedDeployment(RPCError::kRPCNoContractDeployed);
  }

  ExpectedDeployment out;
  out.error(RPCError::kRPCNone);
  out.deployed_hash() = *hash;
  co_return out;
}

x23::Async<ExpectedRetrocausalQueueBlock>
DbExecutorServer::get_next_retrocausal_queue_block(BlockID block,
                                                   AddressAndKey const &addrkey,
                                                   uint64_t search_start_number)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return ExpectedRetrocausalQueueBlock(RPCError::kRPCBadBlockID);
  }

  auto const *item =
      block_data->get_snapshot_view()->get_snapshot_storage_item(addrkey);
  if (!item) {
    co_return ExpectedRetrocausalQueueBlock(RPCError::kRPCNoStorageItem);
  }

  if (item->type() != StorageItemType::kItemTypeRetrocausalQueue) {
    co_return ExpectedRetrocausalQueueBlock(RPCError::kRPCNoStorageItem);
  }

  auto const &queue = item->queue();
  const auto *next_block = queue.try_get_next_block(search_start_number);
  if (!next_block) {
    co_return ExpectedRetrocausalQueueBlock(RPCError::kRPCNoQueueEntry);
  }

  ExpectedRetrocausalQueueBlock out;
  out.error(RPCError::kRPCNone);
  out.queue_block() = *next_block;
  co_return out;
}

x23::Async<ExpectedQueueEntry>
DbExecutorServer::get_concurrent_retrocausal_queue_entry(
    BlockID block, AddressAndKey const &addrkey, uint32_t index)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    co_return ExpectedQueueEntry(RPCError::kRPCBadBlockID);
  }

  auto entry =
      block_data->get_concurrent_retrocausal_queue_entry(addrkey, index);
  if (!entry.get()) {
    co_return ExpectedQueueEntry(RPCError::kRPCNoQueueEntry);
  }

  ExpectedQueueEntry out;
  out.error(RPCError::kRPCNone);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  out.queue_entry() = *entry.get();
  co_return out;
}

} // namespace hogd
