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

void
BlockSnapshotServer::db_snapshot_read_null()
{}

ExpectedStorageItem
BlockSnapshotServer::get_snapshot_item(BlockID block,
                                       AddressAndKey const &addrkey)
{
  if (block != block_id_) {
    return ExpectedStorageItem(RPCError::kRPCBadBlockID);
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
    return ExpectedStorageItem(RPCError::kRPCNoStorageItem);
  }
  ExpectedStorageItem out(RPCError::kRPCNone);
  out.item() = it->second.to_wire_item();
  return out;
}

ExpectedContract
BlockSnapshotServer::get_contract(BlockID block,
                                  SHA256Hash const &contract_hash)
{
  if (block != block_id_) {
    return ExpectedContract(RPCError::kRPCBadBlockID);
  }

  auto m = contracts_.take();
  auto it = m->find(contract_hash);
  if (it == m->end()) {
    return ExpectedContract(RPCError::kRPCContractNotExist);
  }
  ExpectedContract out(RPCError::kRPCNone);
  out.contract() = it->second;
  return out;
}

ExpectedDeployment
BlockSnapshotServer::get_deployment(BlockID block,
                                    ContractAddress const &contract_address)
{
  if (block != block_id_) {
    return ExpectedDeployment(RPCError::kRPCBadBlockID);
  }

  auto m = deployments_.take();
  auto it = m->find(contract_address);
  if (it == m->end()) {
    return ExpectedDeployment(RPCError::kRPCNoContractDeployed);
  }
  ExpectedDeployment out(RPCError::kRPCNone);
  out.deployed_hash() = it->second;
  return out;
}

RPCError
BlockSnapshotServer::commit_contract_create(BlockID /*unused*/,
                                            Contract /*unused*/)
{
  return RPCError::kRPCUnimplemented; // unimplemented
}

RPCError
BlockSnapshotServer::commit_contract_deploy(BlockID block,
                                            ContractAddress const &addr,
                                            SHA256Hash const &hash)
{
  return RPCError::kRPCUnimplemented; // unimplemented
}

RPCError
BlockSnapshotServer::try_push_new_subtransaction(BlockID /*unused*/,
                                                 TransactionID /*unused*/,
                                                 StorageDeltaBatch /*unused*/)
{
  return RPCError::kRPCUnimplemented;
}

RPCError
BlockSnapshotServer::pop_subtransactions(BlockID /*unused*/,
                                         TransactionID /*unused*/,
                                         uint32_t /*unused*/)
{
  return RPCError::kRPCUnimplemented;
}

RPCError
BlockSnapshotServer::commit_transaction(BlockID /*unused*/,
                                        TransactionID /*unused*/)
{
  return RPCError::kRPCUnimplemented; // unimplemented
}

RPCError
BlockSnapshotServer::abort_transaction(BlockID /*unused*/,
                                       TransactionID /*unused*/)
{
  return RPCError::kRPCUnimplemented; // unimplemented;
}

ExpectedRetrocausalQueueBlock
BlockSnapshotServer::get_next_retrocausal_queue_block(
    BlockID block, AddressAndKey const &addrkey, uint64_t search_start_number)
{
  if (block != block_id_) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCBadBlockID);
  }
  auto m = items_.take();
  auto it = m->find(addrkey);
  if (it == m->end()) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNoStorageItem);
  }
  auto &queue = it->second.queue();
  auto const *out = queue.try_get_next_block(search_start_number);
  if (!out) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNoQueueEntry);
  }
  ExpectedRetrocausalQueueBlock blk(RPCError::kRPCNone);
  blk.queue_block() = *out;
  return blk;
}

ExpectedQueueEntry
BlockSnapshotServer::get_concurrent_retrocausal_queue_entry(
    BlockID block, AddressAndKey const &addrkey, uint32_t index)
{
  if (block != block_id_) {
    return ExpectedQueueEntry(RPCError::kRPCBadBlockID);
  }
  auto m = concurrent_rcq_items_.take();
  auto it = m->find(addrkey);
  if (it == m->end()) {
    return ExpectedQueueEntry(RPCError::kRPCNoQueueEntry);
  }
  auto &entries = it->second;
  if (index >= entries.size()) {
    return ExpectedQueueEntry(RPCError::kRPCNoQueueEntry);
  }
  ExpectedQueueEntry out(RPCError::kRPCNone);
  out.queue_entry() = entries[index];
  return out;
}

void
BlockSnapshotServer::setup_create_contract(Contract const &contract)
{
  contracts_->emplace(sha256(contract), contract);
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
