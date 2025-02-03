#include "hogd/coord/executor_manager.h"

#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "hogd/coord/block_frame.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<void>
ExecutorQueue::enqueue_validation_request(BlockID block_id,
                                          BlockValidationFrame *frame)
{
  co_await open_validation_block(block_id);

  co_await queue_.enqueue(
      [this, frame, block_id]() -> x23::Async<void> {
        while (!frame->all_txs_queried() && !frame->validation_failed()) {
          auto const *tx = frame->get_next_validation_transaction();
          if (!tx) {
            break;
          }

          auto res = co_await client_.validate_transaction(block_id, *tx);
          if (res != RPCError::kNone) {
            frame->mark_validation_failed();
            break;
          }
        }
        co_return;
      });

  co_await client_.finish_block(block_id);
}

x23::Async<ExpectedProposalData>
ExecutorQueue::propose_transaction(BlockID block_id,
                                   SignedTransaction const &signed_tx)
{
  return client_.propose_transaction(block_id, signed_tx);
}

x23::Async<bool>
ExecutorQueue::validate_transaction(BlockID block_id,
                                    ProposedTransaction const &proposed_tx)
{
  auto rpc_status =
      co_await client_.validate_transaction(block_id, proposed_tx);
  co_return rpc_status == RPCError::kNone;
}

x23::Async<void>
ExecutorQueue::open_proposal_block(BlockID block_id)
{
  co_await client_.open_proposal_block(block_id);
}

x23::Async<void>
ExecutorQueue::open_validation_block(BlockID block_id)
{
  co_await client_.open_validation_block(block_id);
}

void
ExecutorManager::add_executor(std::unique_ptr<ExecutorQueue> executor)
{
  std::lock_guard lock(mutex_);
  executors_.insert(std::move(executor));
}

x23::Async<void>
ExecutorManager::enqueue_validation_request(BlockID block_id,
                                            BlockValidationFrame *frame)
{
  assert(frame);
  std::vector<x23::Async<void>> results;
  {
    std::shared_lock lock(mutex_);
    assert(!executors_.empty());
    results.reserve(executors_.size());
    for (const auto &executor : executors_) {
      results.push_back(executor->enqueue_validation_request(block_id, frame));
    }
  }

  for (auto &r : results) {
    co_await r;
  }

  frame->mark_validation_finished();

  co_return;
}

} // namespace hogd
