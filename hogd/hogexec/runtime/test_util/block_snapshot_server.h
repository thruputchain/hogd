#pragma once

#include <cerrno>
#include <cstddef>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/storage_item/storage_item_wrapper.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/mvar.h"
#include "x23/srpc.h"

namespace hogd {

// Serve rpcs for runtime tests.
//
// Initialize manually with database snapshot state.
//
// Not a full mock database; extra test logic (as in the lazy loading tests)
// is implemented here.
class BlockSnapshotServer {
public:
  using rpc_service = DatabaseToExecutorInterfaceV1;

  BlockSnapshotServer(BlockID block_id, uint16_t port = 0) : block_id_(block_id)
  {
    lfd_ = x23::stcplisten(port);
    if (!lfd_) {
      perror("listen");
      exit(EXIT_FAILURE);
    }
    port_number_ = x23::get_port(x23::local_addr(lfd_.get()));

    listen_thread_ = std::jthread([this](std::stop_token &&stop) {
      while (!stop.stop_requested()) {

        sockaddr_storage ss;
        socklen_t sslen = sizeof(ss);
        x23::UniqueFd fd{
            accept(lfd_.get(), reinterpret_cast<sockaddr *>(&ss), &sslen)};
        if (!fd) {
          if (errno == EINTR)
            continue;
          // If the listening socket was closed, accept will fail with
          // EBADF/EINVAL Break the loop so the thread can exit.
          if (errno == EBADF || errno == EINVAL)
            break;
          std::println(stderr, "accept: {}", Errno{});
        }
        else {
          x23::srpc_stream_logged(fd.release())->server(this);
        }
      }
    });
  }

  ~BlockSnapshotServer()
  {
    listen_thread_.request_stop();
    shutdown(lfd_.get(), SHUT_RDWR);
    lfd_.reset();
  }

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

  uint16_t port() const { return port_number_; }

private:
  uint16_t port_number_ = 0;
  std::jthread listen_thread_;
  x23::UniqueFd lfd_;

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
