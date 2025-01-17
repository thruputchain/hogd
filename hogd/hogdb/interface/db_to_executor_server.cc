#include "hogd/hogdb/interface/db_to_executor_server.h"

#include "hogd/hogdb/softstate/block_state.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<RPCError>
DBToExecutorServer::try_reserve_delta(BlockID block_id, TransactionID txid,
                                      AddressAndKey const &addrkey,
                                      StorageDelta const &delta)
{
  auto block_data = block_manager_.get_block_data(block_id);

  if (!block_data) {
    co_return RPCError::kBadBlockID;
  }

  auto res = block_data->try_reserve_storage_delta(txid, addrkey, delta);

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

  auto out = ExpectedStorageItem(RPCError::kNone);
  out.item() = *item;

  co_return out;
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
  std::printf("start DBToExecutorServer::get_deployment\n");
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
  std::printf("returning from DBToExecutorServer::get_deployment\n");
  co_return out;
}

} // namespace hogd
