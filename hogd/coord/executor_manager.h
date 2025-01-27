#pragma once

#include <memory>
#include <shared_mutex>
#include <unordered_set>

#include "rpc.hh"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/mvar.h"
#include "x23/rpc.h"

namespace hogd {
class ExecutorQueue {
public:
  using client_t =
      ExecutorInterfaceV1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  ExecutorQueue(uint32_t max_pending_transactions, client_t client)
    : max_pending_transactions_(max_pending_transactions),
      client_(std::move(client))
  {}

  x23::Async<ExpectedProposalData>
  propose_transaction(BlockID block_id, SignedTransaction const &signed_tx);
  x23::Async<bool> validate_transaction(BlockID block_id,
                                        ProposedTransaction const &proposed_tx);

  bool queue_is_full()
  {
    return stats_->pending_transaction_count_ >= max_pending_transactions_;
  }

  void await_block_close(BlockID block_id);

private:
  class ExecutorStats {
  public:
    uint32_t pending_transaction_count_ = 0;

    x23::Async<void> add_proposal_tx(BlockID block_id, client_t &client);
    x23::Async<void> add_validation_tx(BlockID block_id, client_t &client);
    // waits for the executor to finish running on that block.
    // DO NOT REOPEN CLOSED BLOCKS
    void close_block(BlockID block_id, client_t &client);

  private:
    std::unordered_set<BlockID> open_blocks_;
  };

  x23::MVar<ExecutorStats> stats_;
  // Not strictly enforced.  Caller should probably check queue stats before
  // adding a transaction.
  const uint32_t max_pending_transactions_;
  client_t client_;
};

class ExecutorManager {
public:
  void add_executor(std::unique_ptr<ExecutorQueue> executor);
  void await_block_close(BlockID block_id);

private:
  std::shared_mutex mutex_;
  std::unordered_set<std::unique_ptr<ExecutorQueue>> executors_;
};
} // namespace hogd
