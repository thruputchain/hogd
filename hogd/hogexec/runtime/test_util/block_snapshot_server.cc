#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

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
    co_return ExpectedStorageItem(RPCError::kRPCBadBlockID);
  }

  auto m = items_.take();
  auto it = m->find(addrkey);
  if (it == m->end()) {
    co_return ExpectedStorageItem(RPCError::kRPCNoStorageItem);
  }
  ExpectedStorageItem out(RPCError::kRPCNone);
  out.item() = it->second;
  co_return out;
}
x23::Async<ExpectedContract>
BlockSnapshotServer::get_contract(BlockID block,
                                  SHA256Hash const &contract_hash)
{
  if (block != block_id_) {
    co_return ExpectedContract(RPCError::kRPCBadBlockID);
  }

  auto m = contracts_.take();
  auto it = m->find(contract_hash);
  if (it == m->end()) {
    co_return ExpectedContract(RPCError::kRPCContractNotExist);
  }
  ExpectedContract out(RPCError::kRPCNone);
  out.contract() = it->second;
  co_return out;
}

x23::Async<ExpectedDeployment>
BlockSnapshotServer::get_deployment(BlockID block,
                                    ContractAddress const &contract_address)
{
  if (block != block_id_) {
    co_return ExpectedDeployment(RPCError::kRPCBadBlockID);
  }

  auto m = deployments_.take();
  auto it = m->find(contract_address);
  if (it == m->end()) {
    co_return ExpectedDeployment(RPCError::kRPCNoContractDeployed);
  }
  ExpectedDeployment out(RPCError::kRPCNone);
  out.deployed_hash() = it->second;
  co_return out;
}

x23::Async<void>
BlockSnapshotServer::commit_contract_create(BlockID block, Contract contract)
{
  if (block != block_id_) {
    co_return;
  }

  auto h = sha256(contract);
  contracts_->emplace(h, std::move(contract));
}
x23::Async<void>
BlockSnapshotServer::commit_contract_deploy(BlockID block,
                                            ContractAddress const &addr,
                                            SHA256Hash const &hash)
{
  if (block != block_id_) {
    co_return;
  }

  deployments_->emplace(addr, hash);
}

x23::Async<RPCError>
BlockSnapshotServer::push_new_subtransaction(BlockID block, TransactionID tx)
{
  co_return RPCError::kRPCUnimplemented;
}

x23::Async<RPCError>
BlockSnapshotServer::pop_subtransactions(BlockID block, TransactionID tx,
                                         uint32_t popcount)
{
  co_return RPCError::kRPCUnimplemented;
}

x23::Async<ExpectedStorageCost>
BlockSnapshotServer::try_reserve_delta(BlockID block, TransactionID tx,
                                       AddressAndKey addrkey,
                                       StorageDelta delta,
                                       StorageItemRPCFlagSet flags)
{
  co_return ExpectedStorageCost(RPCError::kRPCUnimplemented);
}
x23::Async<void>
BlockSnapshotServer::commit_transaction(BlockID block, TransactionID tx)
{
  co_return; // unimplemented
}
x23::Async<void>
BlockSnapshotServer::abort_transaction(BlockID block, TransactionID tx)
{
  co_return;
  // unimplemented;
}

void
BlockSnapshotServer::start_server(uint32_t port_number)
{
  auto lfd = x23::tcplisten(port_number);
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  [](x23::CoroPoll &cp, BlockSnapshotServer *server,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await cp.pool_.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c).server(server);
    }
  }(cp_, this, std::move(lfd));
}

} // namespace hogd
