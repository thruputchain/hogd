#pragma once

#include <cstddef>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/db_interface/db_executor_server.h"
#include "hogd/rpc/rpc.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/mvar.h"

namespace hogd {

// Serve rpcs for runtime tests.
//
// Initialize manually with database snapshot state.
//
// Not a full mock database; extra test logic (as in the lazy loading tests)
// is implemented here.
class BlockSnapshotServer {
public:
  BlockSnapshotServer(BlockID block_id, uint32_t port = 0) : block_id_(block_id)
  {
    auto res = RPCServer::create(
        port,
        [this](std::span<const std::byte> args) -> OwningSpan<const std::byte> {
          return handle_db_executor_rpc_request(*this, args);
        });
    if (!res) {
      throw std::runtime_error("Failed to create RPC server");
    }
    rpc_server_ = std::move(*res);
    server_listener_ = std::jthread([this] {
      if (ErrnoStatus<void> ret = rpc_server_->listen(); !ret) {
        error("Database to executor server listen failed: {}", ret.error());
      }
    });
  }

  ~BlockSnapshotServer() { rpc_server_->stop(); }

  static void db_snapshot_read_null();

  static RPCError commit_transaction(BlockID block, TransactionID tx);
  static RPCError abort_transaction(BlockID block, TransactionID tx);

  // Create a new subtransaction on top of the current (sub)transaction.
  static RPCError try_push_new_subtransaction(BlockID block, TransactionID tx,
                                              StorageDeltaBatch const &batch);

  // Revert one or more subtransactions.
  static RPCError pop_subtransactions(BlockID block, TransactionID tx,
                                      uint32_t popcount);

  static RPCError commit_contract_create(BlockID block,
                                         Contract const &contract);
  static RPCError commit_contract_deploy(BlockID block,
                                         ContractAddress const &contract_addr,
                                         SHA256Hash const &hash);

  ExpectedStorageItem get_snapshot_item(BlockID block,
                                        AddressAndKey const &addrkey);
  ExpectedContract get_contract(BlockID block, SHA256Hash const &contract_hash);
  ExpectedDeployment get_deployment(BlockID block,
                                    ContractAddress const &contract_address);

  ExpectedRetrocausalQueueBlock
  get_next_retrocausal_queue_block(BlockID block, AddressAndKey const &addrkey,
                                   uint64_t search_start_number);

  ExpectedQueueEntry get_concurrent_retrocausal_queue_entry(
      BlockID block, AddressAndKey const &addrkey, uint32_t index);

  // Only used within test setup
  void setup_create_contract(Contract const &contract);
  void setup_create_deployment(ContractAddress addr, SHA256Hash contract_hash);
  void setup_create_item(AddressAndKey const &addrkey, StorageItemWrapper item);
  void setup_add_concurrent_rcq_item(AddressAndKey const &addrkey,
                                     QueueEntry entry);

  void setup_ban_item(AddressAndKey const &addrkey);

  uint32_t port() const { return rpc_server_->get_port(); }

private:
  std::shared_ptr<RPCServer> rpc_server_;
  std::jthread server_listener_;

  BlockID block_id_;

  x23::MVar<std::unordered_map<AddressAndKey, StorageItemWrapper>> items_;
  x23::MVar<std::unordered_map<SHA256Hash, Contract>> contracts_;
  x23::MVar<std::unordered_map<ContractAddress, SHA256Hash>> deployments_;
  x23::MVar<std::unordered_map<AddressAndKey, std::vector<QueueEntry>>>
      concurrent_rcq_items_;

  // For testing lazy loading:
  // Creates a set of items that throw an error if they are accessed at all
  x23::MVar<std::set<AddressAndKey>> blocked_items_;
};

static_assert(is_DatabaseToExecutorInterfaceV1<BlockSnapshotServer *,
                                               std::type_identity_t>);

} // namespace hogd
