#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"

#include "db_read_interface.hh"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<void>
BlockSnapshotServer::db_snapshot_read_null()
{
  co_return;
}

x23::Async<ExpectedStorageItem>
BlockSnapshotServer::get_snapshot_item(BlockID block,
                                       AddressAndKey const &addrkey)
{
  if (block != block_id_) {
    co_return ExpectedStorageItem(RPCError::kBadBlockID);
  }

  auto m = items_.take();
  auto it = m->find(addrkey);
  if (it == m->end()) {
    co_return ExpectedStorageItem(RPCError::kNoStorageItem);
  }
  ExpectedStorageItem out(RPCError::kNone);
  out.item() = it->second;
  co_return out;
}
x23::Async<ExpectedContract>
BlockSnapshotServer::get_contract(BlockID block,
                                  SHA256Hash const &contract_hash)
{
  if (block != block_id_) {
    co_return ExpectedContract(RPCError::kBadBlockID);
  }

  auto m = contracts_.take();
  auto it = m->find(contract_hash);
  if (it == m->end()) {
    co_return ExpectedContract(RPCError::kContractNotExist);
  }
  ExpectedContract out(RPCError::kNone);
  out.contract() = it->second;
  co_return out;
}

x23::Async<ExpectedDeployment>
BlockSnapshotServer::get_deployment(BlockID block,
                                    ContractAddress const &contract_address)
{
  if (block != block_id_) {
    co_return ExpectedDeployment(RPCError::kBadBlockID);
  }

  auto m = deployments_.take();
  auto it = m->find(contract_address);
  if (it == m->end()) {
    co_return ExpectedDeployment(RPCError::kNoContractDeployed);
  }
  ExpectedDeployment out(RPCError::kNone);
  out.deployed_hash() = it->second;
  co_return out;
}

} // namespace hogd
