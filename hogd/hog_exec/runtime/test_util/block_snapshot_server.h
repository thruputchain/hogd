#pragma once

#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <set>
#include <span>
#include <sys/socket.h>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_db/db_hash.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "network_config.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/mvar.h"

namespace hogd {

// Serve rpcs for runtime tests.
//
// Initialize manually with database snapshot state.
//
// Not a full mock database; extra test logic (as in the lazy loading tests)
// is implemented here.
class BlockSnapshotRpc {
public:
  using rpc_service = DatabaseToExecutorInterfaceV1;

  // A BlockID says nothing about chain position (in production it is a
  // nanosecond timestamp), so the tip height is tracked separately.
  //
  // The served network config defaults to this build's stock parameters.
  // Pass one to place a test or benchmark under a chain that states different
  // execution bounds than this repository's dev chains do.
  BlockSnapshotRpc(BlockID block_id, BlockHeight tip_block_height = 0,
                   NetworkConfig network_config = stock_network_config())
    : block_id_(block_id), tip_block_height_(tip_block_height),
      network_config_(std::move(network_config))
  {}

  // The height the open block claims; what the executor cross-checks against.
  BlockHeight open_block_height() const { return tip_block_height_ + 1; }

  static void db_snapshot_read_null();

  static RPCError commit_transaction(BlockID block, TransactionID tx,
                                     NewContractBatch const &new_contracts);
  static RPCError abort_transaction(BlockID block, TransactionID tx);

  // Create a new subtransaction on top of the current (sub)transaction.
  TryPushSubtransactionResult
  try_push_new_subtransaction(BlockID block, TransactionID tx,
                              StorageDeltaBatch const &batch);

  // Revert one or more subtransactions.
  static RPCError pop_subtransactions(BlockID block, TransactionID tx,
                                      uint32_t popcount);

  x23::Async<ExpectedStorageItem> get_snapshot_item(BlockID block,
                                                    AddressAndKey addrkey);
  ExpectedContract get_contract(BlockID block, SHA256Hash const &contract_hash);
  ExpectedDeployment get_deployment(BlockID block,
                                    ContractAddress const &contract_address);

  ExpectedRetrocausalQueueBlock
  get_next_retrocausal_queue_block(BlockID block, AddressAndKey const &addrkey,
                                   uint64_t search_start_number);

  ExpectedQueueEntry get_concurrent_retrocausal_queue_entry(
      BlockID block, AddressAndKey const &addrkey, uint32_t index);

  ExpectedDBTip get_db_tip(BlockID /*block*/, BlockHeight block_height) const
  {
    // Height-keyed like the real server (executor_to_db_rpc.x): the caller
    // states the height it opened its block with, this server holds exactly
    // one tip, and no per-block state is consulted -- any block opened at
    // the served height reads the same tip, matching the one-committed-tip
    // production shape.
    if (block_height != tip_block_height_ + 1) {
      return ExpectedDBTip(RPCError::kRPCBadBlockID);
    }

    // Fake parent dbtip. get_db_tip returns the *parent* of the open block, so
    // the tip's id must differ from block_id_ (DatabaseProxy::get_db_tip
    // rejects a tip whose id equals the requested block); block_id_ - 1 is
    // just a convenient distinct id, unrelated to the height.
    ExpectedDBTip out;
    out.error(RPCError::kRPCNone);
    ConsensusValue value{};
    value.proposal_header.block_height = tip_block_height_;
    value.proposal_header.txs_hash = sha256(block_id_);
    out.db_tip() =
        DbTip{.id = block_id_ - 1,
              .storage_long_hash = {},
              .value = value,
              // No genesis runs against this server; serve the production
              // native wallet so root-fallback behavior matches a real chain,
              // and the execution parameters this server was built with. A
              // default-constructed NetworkConfig would cap the cross-invoke
              // depth at zero and fault every invoke.
              .default_wallet = native_default_wallet_hash(),
              .network_config = network_config_};
    return out;
  }

  static ExpectedViewingSnapshot open_viewing_snapshot()
  {
    return ExpectedViewingSnapshot(RPCError::kRPCUnimplemented);
  }

  static RPCError close_viewing_snapshot(BlockID /*unused*/)
  {
    return RPCError::kRPCUnimplemented;
  }

  // Only used within test setup
  void setup_create_contract(Contract const &contract);
  void setup_create_deployment(ContractAddress addr,
                               DeployedContractHash deployed_contract_hash);
  void setup_create_item(AddressAndKey const &addrkey, StorageItemWrapper item);
  void setup_add_concurrent_rcq_item(AddressAndKey const &addrkey,
                                     QueueEntry entry);

  void setup_ban_item(AddressAndKey const &addrkey);
  void setup_ban_subtransaction_key(AddressAndKey const &addrkey);
  void setup_stall_item(AddressAndKey const &addrkey);
  void wait_for_stalled_item(AddressAndKey const &addrkey);
  void release_stalled_item(AddressAndKey const &addrkey);

  void shut_down()
  {
    // TODO(jhumphri): Implement.
  }

  void await_shutdown_request()
  {
    // TODO(jhumphri): Implement.
  }

private:
  BlockID block_id_;
  BlockHeight tip_block_height_;
  NetworkConfig network_config_;

  x23::MVar<std::unordered_map<AddressAndKey, StorageItemWrapper>> items_;
  x23::MVar<std::unordered_map<SHA256Hash, Contract, SipHash24Hasher>>
      contracts_;
  x23::MVar<std::unordered_map<ContractAddress, DeployedContractHash,
                               SipHash24Hasher>>
      deployments_;
  x23::MVar<std::unordered_map<AddressAndKey, std::vector<QueueEntry>>>
      concurrent_rcq_items_;

  // For testing lazy loading:
  // Creates a set of items that throw an error if they are accessed at all
  x23::MVar<std::set<AddressAndKey>> blocked_items_;

  // For testing subtransaction key banning:
  x23::MVar<std::set<AddressAndKey>> blocked_subtransaction_keys_;

  // For testing in-flight snapshot access:
  // Reads to these items suspend (via CoroFutex) until released by the test
  // thread. The condvar is only used by wait_for_stalled_item (test thread).
  std::mutex stalled_items_mutex_;
  std::condition_variable stalled_items_cv_;
  std::set<AddressAndKey> items_to_stall_;
  std::set<AddressAndKey> currently_stalled_items_;
  x23::CoroFutex<uint64_t> stall_release_futex_{0};
};
static_assert(
    is_DatabaseToExecutorInterfaceV1<BlockSnapshotRpc *, x23::MaybeAsync>);

class BlockSnapshotServer : public RpcServer<BlockSnapshotRpc> {
public:
  BlockSnapshotServer(BlockID block_id, BlockHeight tip_block_height = 0,
                      NetworkConfig network_config = stock_network_config())
    : RpcServer(block_id, tip_block_height, std::move(network_config))
  {}
};

} // namespace hogd
