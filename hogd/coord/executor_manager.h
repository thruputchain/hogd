#pragma once

#include <memory>
#include <shared_mutex>
#include <unordered_set>

#include "hogd/coord/block_frame.h"
#include "rpc.hh"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/rpc.h"

namespace hogd {
class ExecutorQueue {
public:
  using client_t =
      ExecutorInterfaceV1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  ExecutorQueue(uint32_t max_pending_transactions, client_t client,
                x23::CoroPoll &cp)
    : max_pending_transactions_(max_pending_transactions),
      client_(std::move(client)), cp_(cp)
  {}

  // sets the result by marking frame as validation success or validation
  // failure.
  x23::Async<void> enqueue_validation_request(BlockID block_id,
                                              BlockValidationFrame *frame);

private:
  x23::Async<ExpectedProposalData>
  propose_transaction(BlockID block_id, SignedTransaction const &signed_tx);
  x23::Async<bool> validate_transaction(BlockID block_id,
                                        ProposedTransaction const &proposed_tx);

  x23::Async<void> open_proposal_block(BlockID block_id);
  x23::Async<void> open_validation_block(BlockID block_id);

  class ExecutorStats {
  public:
    x23::CoroLock lock_;
    uint32_t running_coroutine_count_ = 0;
    std::vector<x23::CoroCB<void>> pending_coroutines_;

    // should be called with lock_ held
    std::coroutine_handle<> awake_one_pending();
    std::vector<std::coroutine_handle<>> awake_all_pending();
  };

  ExecutorStats stats_;
  // Not strictly enforced.  Caller should probably check queue stats before
  // adding a transaction.
  const uint32_t max_pending_transactions_;
  client_t client_;
  x23::CoroPoll &cp_;
};

class ExecutorManager {
public:
  void add_executor(std::unique_ptr<ExecutorQueue> executor);

  x23::Async<void> enqueue_validation_request(BlockID block_id,
                                              BlockValidationFrame *frame);

private:
  std::shared_mutex mutex_;
  std::unordered_set<std::unique_ptr<ExecutorQueue>> executors_;
};
} // namespace hogd
