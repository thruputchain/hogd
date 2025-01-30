#include "hogd/coord/executor_manager.h"

#include <mutex>

#include "rpc_types.hh"
#include "x23/async.h"

namespace hogd {

std::coroutine_handle<>
ExecutorQueue::ExecutorStats::awake_one_pending()
{
  if (pending_coroutines_.empty())
    return nullptr;
  auto back = pending_coroutines_.back();
  pending_coroutines_.pop_back();

  return back.set_result_without_resuming();
}

std::vector<std::coroutine_handle<>>
ExecutorQueue::ExecutorStats::awake_all_pending()
{
  std::vector<std::coroutine_handle<>> handles;
  while (!pending_coroutines_.empty()) {
    if (auto h = awake_one_pending()) {
      handles.push_back(h);
    }
  }
  return handles;
}

x23::Async<void>
ExecutorQueue::enqueue_validation_request(BlockID block_id,
                                          BlockValidationFrame *frame)
{
  co_await open_validation_block(block_id);

  std::vector<x23::Async<void>> results;

  auto validation_coro = [this, frame, block_id]() -> x23::Async<void> {
    co_await cp_.pool_.bg();

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

    x23::CoroGuard g;
    co_await g(stats_.lock_);
    stats_.running_coroutine_count_--;

    auto handles = stats_.awake_all_pending();
    g.reset();

    for (auto h : handles) {
      if (h) {
        h.resume();
      }
    }

    co_return;
  };

  auto enqueue_coro = [this, frame, &results,
                       &validation_coro] -> x23::Async<void> {
    co_await cp_.pool_.bg();

    while (true) {
      x23::CoroGuard g;
      co_await g(stats_.lock_);

      if (frame->all_txs_queried() || frame->validation_failed()) {
        co_return;
      }

      if (stats_.running_coroutine_count_ < max_pending_transactions_) {
        stats_.running_coroutine_count_++;
        results.push_back(validation_coro());
        continue;
      }

      auto [async, cb] = x23::make_async<void>();
      stats_.pending_coroutines_.push_back(cb);
      g.reset();

      co_await async;
    }
    co_return;
  };

  co_await enqueue_coro();

  co_await client_.finish_block(block_id);

  for (auto &r : results) {
    co_await r;
  }

  co_return;
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
  std::vector<x23::Async<void>> results;
  {
    std::shared_lock lock(mutex_);
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
