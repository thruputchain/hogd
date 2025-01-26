#include "hogd/hogdb/interface/db_to_executor_server.h"

#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<RPCError>
DBToExecutorServer::try_reserve_delta(BlockID block_id, TransactionID txid,
                                      AddressAndKey const &addrkey,
                                      StorageDelta const &delta,
                                      RPCFlagSet flags)
{
  auto block_data = block_manager_.get_block_data(block_id);

  if (!block_data) {
    co_return RPCError::kBadBlockID;
  }

  auto res = block_data->try_reserve_storage_delta(txid, addrkey, delta, flags);

  if (res) {
    co_return RPCError::kNone;
  }
  co_return RPCError::kStorageWriteFailed;
}

x23::Async<void>
DBToExecutorServer::commit_transaction(BlockID block_id, TransactionID txid)
{
  auto block_data = block_manager_.get_block_data(block_id);
  if (!block_data) {
    co_return;
  }

  block_data->commit_transaction(txid);
  co_return;
}
x23::Async<void>
DBToExecutorServer::abort_transaction(BlockID block_id, TransactionID txid)
{
  auto block_data = block_manager_.get_block_data(block_id);
  if (!block_data) {
    co_return;
  }

  block_data->abort_transaction(txid);
  co_return;
}

x23::Async<ExpectedStorageItem>
DBToExecutorServer::get_item(BlockID block_id, AddressAndKey const &addrkey)
{
  auto block_data = block_manager_.get_block_data(block_id);
  if (!block_data) {
    co_return ExpectedStorageItem(RPCError::kBadBlockID);
  }

  auto item = block_data->get_snapshot_storage_item(addrkey);
  if (!item) {
    co_return ExpectedStorageItem(RPCError::kNoStorageItem);
  }
  assert(item->type() != StorageItemType::kDeleted);

  auto out = ExpectedStorageItem(RPCError::kNone);
  out.item() = item->to_wire_item();

  co_return out;
}

x23::Async<ExpectedRetrocausalQueueBlock>
DBToExecutorServer::get_next_retrocausal_queue_block(
    BlockID block_id, AddressAndKey const &addrkey, uint64_t block_number)
{
  co_return ExpectedRetrocausalQueueBlock(RPCError::kNoStorageItem);
}

x23::Async<ExpectedQueueEntry>
DBToExecutorServer::get_concurrent_retrocausal_queue_entry(
    BlockID block_id, AddressAndKey const &addrkey, uint32_t index)
{
  co_return ExpectedQueueEntry(RPCError::kNoStorageItem);
}

x23::Async<void>
DBToExecutorServer::commit_contract_create(BlockID block_id,
                                           Contract const &contract)
{
  auto block_data = block_manager_.get_block_data(block_id);
  if (!block_data) {
    co_return;
  }

  block_data->commit_contract_create(contract);
  co_return;
}

x23::Async<void>
DBToExecutorServer::commit_contract_deploy(BlockID block_id,
                                           ContractAddress const &addr,
                                           SHA256Hash const &contract_hash)
{
  auto block_data = block_manager_.get_block_data(block_id);
  if (!block_data) {
    co_return;
  }

  block_data->commit_contract_deploy(addr, contract_hash);
  co_return;
}

x23::Async<ExpectedContract>
DBToExecutorServer::get_contract(BlockID block_id,
                                 SHA256Hash const &contract_hash)
{
  auto block_data = block_manager_.get_block_data(block_id);
  if (!block_data) {
    co_return ExpectedContract(RPCError::kBadBlockID);
  }

  auto contract = block_data->get_snapshot_contract(contract_hash);
  if (!contract) {
    co_return ExpectedContract(RPCError::kContractNExist);
  }

  auto out = ExpectedContract(RPCError::kNone);
  out.contract() = *contract;
  co_return out;
}

x23::Async<ExpectedDeployment>
DBToExecutorServer::get_deployment(BlockID block_id,
                                   ContractAddress const &addr)
{
  auto block_data = block_manager_.get_block_data(block_id);
  if (!block_data) {
    co_return ExpectedDeployment(RPCError::kBadBlockID);
  }

  auto hash = block_data->get_snapshot_deployment(addr);
  if (!hash) {
    co_return ExpectedDeployment(RPCError::kNoContractDeployed);
  }

  auto out = ExpectedDeployment(RPCError::kNone);
  out.deployed_hash() = *hash;
  co_return out;
}

} // namespace hogd
