#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <exception>
#include <sys/socket.h>
#include <type_traits>

// generated x23 headers
#include "db_conflict.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/hog_db/invoke_tx.h"
#include "hogd/hog_db/preserialized_reply.h"
#include "hogd/hog_db/storage_item_reply.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/rpc_context.h"
#include "retrocausal_queue.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

// Reply for get_concurrent_retrocausal_queue_entry that borrows the lock's
// bytes and holds the block-tx reference keeping them alive until x23 has
// serialized them.  Move-only: that reference must be released exactly once.
class BorrowedQueueEntryReply {
public:
  // A reply that borrows nothing and holds no reference.
  explicit BorrowedQueueEntryReply(RPCError error) noexcept : error_(error) {}

  // Takes over the reference get_block_tx acquired on [btx], which is null
  // when no block tx was found.  Answers kRPCNotFound until told otherwise.
  BorrowedQueueEntryReply(RPCContext &ctx, PerThreadARC<BlockTX> *btx,
                          BlockID block) noexcept
    : ctx_(&ctx), btx_(btx), block_(block)
  {}

  BorrowedQueueEntryReply(BorrowedQueueEntryReply &&other) noexcept
    : error_(other.error_), entry_(other.entry_), ctx_(other.ctx_),
      btx_(other.btx_), block_(other.block_)
  {
    other.btx_ = nullptr;
  }
  BorrowedQueueEntryReply(const BorrowedQueueEntryReply &) = delete;
  BorrowedQueueEntryReply &operator=(BorrowedQueueEntryReply &&) = delete;
  BorrowedQueueEntryReply &operator=(const BorrowedQueueEntryReply &) = delete;

  ~BorrowedQueueEntryReply()
  {
    try {
      if (btx_ && btx_->release_reference()) {
        ctx_->remove_shutdown_block_tx(block_);
      }
    } catch (...) {
      std::terminate();
    }
  }

  // Answer with [error] and no entry.
  void fail(RPCError error) noexcept { error_ = error; }

  // Answer with [entry], borrowed from the block tx this reply holds.
  void set_entry(x23::BufView entry) noexcept
  {
    // Deserialization bounded this on the way in, so a longer entry is a
    // broken invariant rather than bad input.
    CHECK(entry.size() <= kQueueEntryMaxLen);
    error_ = RPCError::kRPCNone;
    entry_ = entry;
  }

  RPCError error() const noexcept { return error_; }
  x23::BufView entry() const noexcept { return entry_; }

private:
  RPCError error_ = RPCError::kRPCNotFound;
  x23::BufView entry_;
  // Set together: ctx_ is read only to release btx_.
  RPCContext *ctx_ = nullptr;
  PerThreadARC<BlockTX> *btx_ = nullptr;
  BlockID block_ = 0;
};

// As x23 writes the generated union: discriminant, then the active arm.
// That arm is the entry on kRPCNone, nothing otherwise.
inline size_t
xdr_size(const BorrowedQueueEntryReply &reply)
{
  size_t size = 4; // discriminant
  if (reply.error() == RPCError::kRPCNone) {
    size += x23::xdr_size(reply.entry());
  }
  return size;
}

inline void
xdr_serialize(auto &serializer, const BorrowedQueueEntryReply &reply)
{
  serializer.put32(static_cast<uint32_t>(reply.error()));
  if (reply.error() == RPCError::kRPCNone) {
    serializer(reply.entry());
  }
}

} // namespace hogd

namespace x23 {
// Declares BorrowedQueueEntryReply a stand-in for the ExpectedQueueEntry the
// RPC interface declares.  Must precede the is_DatabaseToExecutorInterfaceV1
// assertion below, which is what checks it.
template<>
struct serialize_equiv<hogd::ExpectedQueueEntry, hogd::BorrowedQueueEntryReply>
  : std::true_type {};
} // namespace x23

namespace hogd {

// Core logic handler for the db rpc server.
class HogDbExecutorRpc {
public:
  using rpc_service = DatabaseToExecutorInterfaceV1;

  explicit HogDbExecutorRpc(RPCContext &ctx) : ctx_(ctx) {}

  RPCError commit_transaction(BlockID block, TransactionID tx,
                              NewContractBatch new_contracts);
  RPCError abort_transaction(BlockID block, TransactionID tx);
  TryPushSubtransactionResult
  try_push_new_subtransaction(BlockID block, TransactionID tx,
                              StorageDeltaBatch batch);
  RPCError pop_subtransactions(BlockID block, TransactionID tx,
                               uint32_t popcount);

  StorageItemReply<ExpectedStorageItem>
  get_snapshot_item(BlockID block, AddressAndKey const &addrkey);

  // Serves the stored row as is: it already is the Contract's encoding.
  PreserializedReply<ExpectedContract> get_contract(BlockID block,
                                                    SHA256Hash const &hash);
  ExpectedDeployment get_deployment(BlockID block,
                                    ContractAddress const &contract_address);
  ExpectedRetrocausalQueueBlock
  get_next_retrocausal_queue_block(BlockID block, AddressAndKey const &addrkey,
                                   uint64_t search_start_number);
  BorrowedQueueEntryReply get_concurrent_retrocausal_queue_entry(
      BlockID block, AddressAndKey const &addrkey, uint32_t index);

  // A coroutine: it may suspend to wait out a commit publication, and the
  // rpc server runs handlers on ThreadPool workers, which must not park.
  x23::Async<ExpectedDBTip> get_db_tip(BlockID block, BlockHeight block_height);

  // A coroutine for the same reason as get_db_tip(): it may suspend on a
  // commit publication.
  x23::Async<ExpectedViewingSnapshot> open_viewing_snapshot();
  RPCError close_viewing_snapshot(BlockID block);

  void shut_down()
  {
    // TODO(jhumphri): Implement.
    shutdown_flag_ = true;
  }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
  RPCContext &ctx_;

  static RPCError request_one(InvokeTX &tx, AddressAndKey const &addrkey,
                              StorageDelta &&delta, StorageDeltaFlagSet flags,
                              DbConflictCode *conflict_code);
};
// MaybeAsync rather than type_identity_t: get_db_tip and
// open_viewing_snapshot are coroutines (they can suspend on a commit
// publication), which is also the transform RpcServer itself validates.
static_assert(
    is_DatabaseToExecutorInterfaceV1<HogDbExecutorRpc *, x23::MaybeAsync>);

class HogDbExecutorServer : public RpcServer<HogDbExecutorRpc> {
public:
  explicit HogDbExecutorServer(RPCContext &ctx) : RpcServer(ctx)
  {
    // New contract bodies commit in one message, so this link is sized by the
    // aggregate a transaction is admitted with, not a per-message budget.
    max_message_size_ = kBulkRpcMaxMessageSize;
  }
};

} // namespace hogd
