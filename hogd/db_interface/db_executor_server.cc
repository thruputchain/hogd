#include "hogd/db_interface/db_executor_server.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/types.h"
#include "hogd/db_interface/block_state.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

// SyncDBExecutorServer implementations

bool
SyncDBExecutorServer::try_reserve_delta(BlockState *block_state,
                                        TransactionID tx,
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
SyncDBExecutorServer::try_push_new_subtransaction(
    BlockID block, TransactionID tx, StorageDeltaBatch const &batch)
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
SyncDBExecutorServer::get_snapshot_item(BlockID block,
                                        AddressAndKey const &addrkey)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return ExpectedStorageItem(RPCError::kRPCBadBlockID);
  }

  auto const *item =
      block_data->get_snapshot_view()->get_snapshot_storage_item(addrkey);
  if (!item) {
    return ExpectedStorageItem(RPCError::kRPCNotFound);
  }

  ExpectedStorageItem out;
  out.error(RPCError::kRPCNone);
  out.item() = item->to_wire_item();
  return out;
}

RPCError
SyncDBExecutorServer::commit_contract_create(BlockID block,
                                             Contract const &contract)
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
SyncDBExecutorServer::get_contract(BlockID block,
                                   SHA256Hash const &contract_hash)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return ExpectedContract(RPCError::kRPCBadBlockID);
  }

  auto contract =
      block_data->get_snapshot_view()->get_snapshot_contract(contract_hash);
  if (!contract) {
    return ExpectedContract(RPCError::kRPCNotFound);
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
    return ExpectedDeployment(RPCError::kRPCNotFound);
  }

  ExpectedDeployment out;
  out.error(RPCError::kRPCNone);
  out.deployed_hash() = *hash;
  return out;
}

ExpectedRetrocausalQueueBlock
SyncDBExecutorServer::get_next_retrocausal_queue_block(
    BlockID block, AddressAndKey const &addrkey, uint64_t search_start_number)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCBadBlockID);
  }

  auto const *item =
      block_data->get_snapshot_view()->get_snapshot_storage_item(addrkey);
  if (!item) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNotFound);
  }

  if (item->type() != StorageItemType::kItemTypeRetrocausalQueue) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNotFound);
  }

  auto const &queue = item->queue();
  const auto *next_block = queue.try_get_next_block(search_start_number);
  if (!next_block) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNotFound);
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
    return ExpectedQueueEntry(RPCError::kRPCNotFound);
  }

  ExpectedQueueEntry out;
  out.error(RPCError::kRPCNone);
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  out.queue_entry() = *entry.get();
  return out;
}

} // namespace hogd
