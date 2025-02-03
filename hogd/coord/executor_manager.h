#pragma once

#include <memory>
#include <shared_mutex>
#include <unordered_set>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/coord/block_frame.h"
#include "hogd/coord/coro_queue.h"
#include "rpc.hh"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"

namespace hogd {
class ExecutorQueue {
public:
  using client_t =
      ExecutorInterfaceV1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  ExecutorQueue(uint32_t max_pending_transactions, client_t client,
                x23::CoroPoll &cp)
    : queue_(max_pending_transactions, cp), client_(std::move(client))
  {}

  // sets the result by marking frame as validation success or validation
  // failure.
  x23::Async<void> enqueue_validation_request(BlockID block_id,
                                              BlockValidationFrame *frame);

  ~ExecutorQueue();

private:
  x23::Async<ExpectedProposalData>
  propose_transaction(BlockID block_id, SignedTransaction const &signed_tx);
  x23::Async<bool> validate_transaction(BlockID block_id,
                                        ProposedTransaction const &proposed_tx);

  x23::Async<void> open_proposal_block(BlockID block_id);
  x23::Async<void> open_validation_block(BlockID block_id);

  CoroQueue queue_;
  client_t client_;
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
