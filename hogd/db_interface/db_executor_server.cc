#include "hogd/db_interface/db_executor_server.h"

#include "executor_to_db_rpc.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/db_interface/block_state.h"
#include "hogd/debug/key_type_check.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include <cassert>
#include <set>
#include <utility>

namespace hogd {

// DbExecutorRpc implementations

bool
DbExecutorRpc::try_reserve_delta(BlockState *block_state, TransactionID tx,
                                 AddressAndKey const &addrkey,
                                 StorageDelta const &delta,
                                 StorageDeltaFlagSet flags)
{
  if constexpr (is_debug_build()) {
    check_delta_type(addrkey.key.key_type(), delta.type());
  }
  // Reject exclusive locks on replay cache items (match HogDB behavior).
  if (delta.type() == StorageDeltaType::kDeltaTypeConsumeUniquifier &&
      (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock))) {
    return false;
  }
  // MockInMemory does not implement the replay cache (it is being removed).
  // Accept-and-drop consume-uniquifier deltas so a transaction that consumes
  // its nonce still commits against the mock; replay semantics (dedup, expiry,
  // capacity) are verified against HogDB only.
  if (delta.type() == StorageDeltaType::kDeltaTypeConsumeUniquifier) {
    return true;
  }
  // Reject exclusive locks on balance keys (match HogDB behavior).
  if (addrkey.key.key_type() == StorageKeyType::FreeNativeTokenBalance &&
      (flags & std::to_underlying(StorageDeltaFlags::ExclusiveLock))) {
    return false;
  }
  return block_state->try_reserve_storage_delta(tx, addrkey, delta, flags);
}

RPCError
DbExecutorRpc::commit_transaction(BlockID block, TransactionID tx,
                                  NewContractBatch new_contracts)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return RPCError::kRPCBadBlockID;
  }

  block_data->commit_transaction(tx, std::move(new_contracts));
  return RPCError::kRPCNone;
}

RPCError
DbExecutorRpc::abort_transaction(BlockID block, TransactionID tx)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return RPCError::kRPCBadBlockID;
  }

  block_data->abort_transaction(tx);
  return RPCError::kRPCNone;
}

TryPushSubtransactionResult
DbExecutorRpc::try_push_new_subtransaction(BlockID block, TransactionID tx,
                                           StorageDeltaBatch const &batch)
{
  // test double: produces no diagnostics
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return TryPushSubtransactionResult(RPCError::kRPCBadBlockID);
  }

  block_data->push_subtransaction(tx);

  if constexpr (is_debug_build()) {
    // Invariant: hogexec sends at most one delta per key per subtransaction.
    std::set<AddressAndKey> seen_keys;
    for (auto const &delta : batch) {
      auto [_, inserted] = seen_keys.insert(delta.addrkey);
      CHECK(inserted);
    }
  }

  for (auto const &delta : batch) {
    if (!try_reserve_delta(block_data.get(), tx, delta.addrkey, delta.delta,
                           delta.flags)) {
      if (auto err = block_data->pop_subtransactions(tx, 1);
          err != RPCError::kRPCNone) {
        return TryPushSubtransactionResult(err);
      }
      return TryPushSubtransactionResult(RPCError::kRPCStorageWriteFailed);
    }
  }

  return TryPushSubtransactionResult(RPCError::kRPCNone);
}

RPCError
DbExecutorRpc::pop_subtransactions(BlockID block, TransactionID tx,
                                   uint32_t popcount)
{
  auto block_data = block_manager_->get_block_data(block);
  if (!block_data) {
    return RPCError::kRPCBadBlockID;
  }

  return block_data->pop_subtransactions(tx, popcount);
}

ExpectedStorageItem
DbExecutorRpc::get_snapshot_item(BlockID block, AddressAndKey const &addrkey)
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

ExpectedContract
DbExecutorRpc::get_contract(BlockID block, SHA256Hash const &contract_hash)
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
DbExecutorRpc::get_deployment(BlockID block,
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
DbExecutorRpc::get_next_retrocausal_queue_block(BlockID block,
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
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNotFound);
  }

  if (item->type() != StorageItemType::kItemTypeRetrocausalQueue) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNotFound);
  }

  auto const &queue = item->queue();
  const auto *next_block = queue.try_get_block(search_start_number);
  if (!next_block) {
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCNotFound);
  }

  ExpectedRetrocausalQueueBlock out;
  out.error(RPCError::kRPCNone);
  out.queue_block() = *next_block;
  return out;
}

ExpectedQueueEntry
DbExecutorRpc::get_concurrent_retrocausal_queue_entry(
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

ExpectedDBTip
DbExecutorRpc::get_db_tip(BlockID block_id)
{
  auto block_data = block_manager_->get_block_data(block_id);
  if (!block_data) {
    return ExpectedDBTip(RPCError::kRPCBadBlockID);
  }

  // The mock does not track block height, so report the pre-genesis sentinel
  // height (-1). At executing height 0 this makes the executor's replay-expiry
  // gate a no-op, consistent with the mock not implementing the replay cache.
  // The tip id is the committed parent -- distinct from block_id, which
  // get_block_data() just confirmed is an open child (DatabaseProxy rejects a
  // tip whose id equals the requested block).
  ExpectedDBTip out;
  out.error(RPCError::kRPCNone);
  out.db_tip() = DbTip{
      .height = -1,
      .block_hash = {},
      .db_hash = {},
      .id = block_manager_->committed_tip_id(),
  };
  return out;
}

} // namespace hogd
