#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
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

  {
    auto blocked = blocked_items_.take();
    if (blocked->contains(addrkey)) {
      throw std::runtime_error("Blocked item accessed");
    }
  }

  auto m = items_.take();
  auto it = m->find(addrkey);
  if (it == m->end()) {
    co_return ExpectedStorageItem(RPCError::kRPCNoStorageItem);
  }
  ExpectedStorageItem out(RPCError::kRPCNone);
  out.item() = it->second.to_wire_item();
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

x23::Async<RPCError>
BlockSnapshotServer::commit_contract_create(BlockID block, Contract contract)
{
  co_return RPCError::kRPCUnimplemented; // unimplemented
}
x23::Async<RPCError>
BlockSnapshotServer::commit_contract_deploy(BlockID block,
                                            ContractAddress const &addr,
                                            SHA256Hash const &hash)
{
  co_return RPCError::kRPCUnimplemented; // unimplemented
}

x23::Async<RPCError>
BlockSnapshotServer::try_push_new_subtransaction(BlockID block,
                                                 TransactionID tx,
                                                 StorageDeltaBatch batch)
{
  co_return RPCError::kRPCUnimplemented;
}

x23::Async<RPCError>
BlockSnapshotServer::pop_subtransactions(BlockID block, TransactionID tx,
                                         uint32_t popcount)
{
  co_return RPCError::kRPCUnimplemented;
}

x23::Async<RPCError>
BlockSnapshotServer::commit_transaction(BlockID block, TransactionID tx)
{
  co_return RPCError::kRPCUnimplemented; // unimplemented
}
x23::Async<RPCError>
BlockSnapshotServer::abort_transaction(BlockID block, TransactionID tx)
{
  co_return RPCError::kRPCUnimplemented;
  // unimplemented;
}

sockaddr_storage
BlockSnapshotServer::start_server(uint32_t port_number)
{
  auto lfd = x23::tcplisten(port_number);
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  auto out = x23::local_addr(lfd);

  [](x23::CoroPoll &cp, BlockSnapshotServer *server,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await cp.pool_.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c).server(server);
    }
  }(cp_, this, std::move(lfd));

  return out;
}

x23::Async<ExpectedRetrocausalQueueBlock>
BlockSnapshotServer::get_next_retrocausal_queue_block(
    BlockID block, AddressAndKey const &addrkey, uint64_t search_start_number)
{
  if (block != block_id_) {
    co_return ExpectedRetrocausalQueueBlock(RPCError::kRPCBadBlockID);
  }
  auto m = items_.take();
  auto it = m->find(addrkey);
  if (it == m->end()) {
    co_return ExpectedRetrocausalQueueBlock(RPCError::kRPCNoStorageItem);
  }
  auto &queue = it->second.queue();
  auto const *out = queue.try_get_next_block(search_start_number);
  if (!out) {
    co_return ExpectedRetrocausalQueueBlock(RPCError::kRPCNoQueueEntry);
  }
  ExpectedRetrocausalQueueBlock blk(RPCError::kRPCNone);
  blk.queue_block() = *out;
  co_return blk;
}

x23::Async<ExpectedQueueEntry>
BlockSnapshotServer::get_concurrent_retrocausal_queue_entry(
    BlockID block, AddressAndKey const &addrkey, uint32_t index)
{
  auto m = concurrent_rcq_items_.take();
  auto it = m->find(addrkey);
  if (it == m->end()) {
    co_return ExpectedQueueEntry(RPCError::kRPCNoQueueEntry);
  }
  auto &entries = it->second;
  if (index >= entries.size()) {
    co_return ExpectedQueueEntry(RPCError::kRPCNoQueueEntry);
  }
  ExpectedQueueEntry out(RPCError::kRPCNone);
  out.queue_entry() = entries[index];
  co_return out;
}

void
BlockSnapshotServer::setup_create_contract(Contract const &contract)
{
  auto h = LibCrypto::sha256(contract);
  contracts_->emplace(h, contract);
}
void
BlockSnapshotServer::setup_create_deployment(ContractAddress addr,
                                             SHA256Hash contract_hash)
{
  deployments_->emplace(addr, contract_hash);
}

void
BlockSnapshotServer::setup_create_item(AddressAndKey const &addrkey,
                                       StorageItemWrapper item)
{
  items_->emplace(addrkey, std::move(item));
}

void
BlockSnapshotServer::setup_add_concurrent_rcq_item(AddressAndKey const &addrkey,
                                                   QueueEntry entry)
{
  auto it = concurrent_rcq_items_->find(addrkey);
  if (it == concurrent_rcq_items_->end()) {
    concurrent_rcq_items_->emplace(addrkey,
                                   std::vector<QueueEntry>{std::move(entry)});
  }
  else {
    it->second.push_back(entry);
  }
}

void
BlockSnapshotServer::setup_ban_item(AddressAndKey const &addrkey)
{
  blocked_items_->insert(addrkey);
}

} // namespace hogd
