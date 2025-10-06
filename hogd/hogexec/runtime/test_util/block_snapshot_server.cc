#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"

#include <stdexcept>
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

namespace hogd {

void
BlockSnapshotRpc::db_snapshot_read_null()
{}

ExpectedStorageItem
BlockSnapshotRpc::get_snapshot_item(BlockID block, AddressAndKey const &addrkey)
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
    return ExpectedStorageItem(RPCError::kRPCNotFound);
  }
  ExpectedStorageItem out(RPCError::kRPCNone);
  out.item() = it->second.to_wire_item();
  return out;
}

ExpectedContract
BlockSnapshotRpc::get_contract(BlockID block, SHA256Hash const &contract_hash)
{
  if (block != block_id_) {
    return ExpectedContract(RPCError::kRPCBadBlockID);
  }

  auto m = contracts_.take();
  auto it = m->find(contract_hash);
  if (it == m->end()) {
    return ExpectedContract(RPCError::kRPCNotFound);
  }
  ExpectedContract out(RPCError::kRPCNone);
  out.contract() = it->second;
  return out;
}

ExpectedDeployment
BlockSnapshotRpc::get_deployment(BlockID block,
                                 ContractAddress const &contract_address)
{
  if (block != block_id_) {
    return ExpectedDeployment(RPCError::kRPCBadBlockID);
  }

  auto m = deployments_.take();
  auto it = m->find(contract_address);
  if (it == m->end()) {
    return ExpectedDeployment(RPCError::kRPCNotFound);
  }
  ExpectedDeployment out(RPCError::kRPCNone);
  out.deployed_hash() = it->second;
  return out;
}

RPCError
BlockSnapshotRpc::try_push_new_subtransaction(
    BlockID /*unused*/, TransactionID /*unused*/,
    StorageDeltaBatch const & /*unused*/)
{
  return RPCError::kRPCUnimplemented;
}

RPCError
BlockSnapshotRpc::pop_subtransactions(BlockID /*unused*/,
                                      TransactionID /*unused*/,
                                      uint32_t /*unused*/)
{
  return RPCError::kRPCUnimplemented;
}

RPCError
BlockSnapshotRpc::commit_transaction(BlockID /*unused*/,
                                     TransactionID /*unused*/,
                                     NewContractBatch const & /*unused*/)
{
  return RPCError::kRPCUnimplemented; // unimplemented
}

RPCError
BlockSnapshotRpc::abort_transaction(BlockID /*unused*/,
                                    TransactionID /*unused*/)
{
  return RPCError::kRPCUnimplemented; // unimplemented;
}

ExpectedRetrocausalQueueBlock
BlockSnapshotRpc::get_next_retrocausal_queue_block(BlockID block,
                                                   AddressAndKey const &addrkey,
                                                   uint64_t search_start_number)
{
  if (block != block_id_) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCBadBlockID);
  }
  auto m = items_.take();
  auto it = m->find(addrkey);
  if (it == m->end()) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNotFound);
  }
  auto &queue = it->second.queue();
  auto const *out = queue.try_get_next_block(search_start_number);
  if (!out) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNotFound);
  }
  ExpectedRetrocausalQueueBlock blk(RPCError::kRPCNone);
  blk.queue_block() = *out;
  return blk;
}

ExpectedQueueEntry
BlockSnapshotRpc::get_concurrent_retrocausal_queue_entry(
    BlockID block, AddressAndKey const &addrkey, uint32_t index)
{
  if (block != block_id_) {
    return ExpectedQueueEntry(RPCError::kRPCBadBlockID);
  }
  auto m = concurrent_rcq_items_.take();
  auto it = m->find(addrkey);
  if (it == m->end()) {
    return ExpectedQueueEntry(RPCError::kRPCNotFound);
  }
  auto &entries = it->second;
  if (index >= entries.size()) {
    return ExpectedQueueEntry(RPCError::kRPCNotFound);
  }
  ExpectedQueueEntry out(RPCError::kRPCNone);
  out.queue_entry() = entries[index];
  return out;
}

void
BlockSnapshotRpc::setup_create_contract(Contract const &contract)
{
  contracts_->emplace(sha256(contract), contract);
}

void
BlockSnapshotRpc::setup_create_deployment(ContractAddress addr,
                                          SHA256Hash contract_hash)
{
  deployments_->emplace(addr, contract_hash);
}

void
BlockSnapshotRpc::setup_create_item(AddressAndKey const &addrkey,
                                    StorageItemWrapper item)
{
  items_->emplace(addrkey, std::move(item));
}

void
BlockSnapshotRpc::setup_add_concurrent_rcq_item(AddressAndKey const &addrkey,
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
BlockSnapshotRpc::setup_ban_item(AddressAndKey const &addrkey)
{
  blocked_items_->insert(addrkey);
}

} // namespace hogd
