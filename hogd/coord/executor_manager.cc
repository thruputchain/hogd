#include "hogd/coord/executor_manager.h"

#include "rpc_types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<ExpectedProposalData>
ExecutorQueue::propose_transaction(BlockID block_id,
                                   SignedTransaction const &signed_tx)
{
  co_await stats_->add_proposal_tx(block_id, client_);
  auto res = co_await client_.propose_transaction(block_id, signed_tx);
  stats_->pending_transaction_count_--;
  co_return res;
}

x23::Async<bool>
ExecutorQueue::validate_transaction(BlockID block_id,
                                    ProposedTransaction const &proposed_tx)
{
  co_await stats_->add_validation_tx(block_id, client_);
  auto rpc_status =
      co_await client_.validate_transaction(block_id, proposed_tx);
  stats_->pending_transaction_count_--;
  co_return rpc_status == RPCError::kNone;
}

x23::Async<void>
ExecutorQueue::ExecutorStats::add_proposal_tx(BlockID block_id,
                                              client_t &client)
{
  auto it = open_blocks_.insert(block_id);
  if (it.second) {
    co_await client.open_proposal_block(block_id);
  }

  pending_transaction_count_++;
}

x23::Async<void>
ExecutorQueue::ExecutorStats::add_validation_tx(BlockID block_id,
                                                client_t &client)
{
  auto it = open_blocks_.insert(block_id);
  if (it.second) {
    co_await client.open_validation_block(block_id);
  }
  pending_transaction_count_++;
}

void
ExecutorQueue::ExecutorStats::close_block(BlockID block_id, client_t &client)
{
  open_blocks_.erase(block_id);
  client.finish_block(block_id)();
}

void
ExecutorQueue::await_block_close(BlockID block_id)
{
  stats_->close_block(block_id, client_);
}

void
ExecutorManager::add_executor(std::unique_ptr<ExecutorQueue> executor)
{
  std::lock_guard lock(mutex_);
  executors_.insert(std::move(executor));
}

void
ExecutorManager::await_block_close(BlockID block_id)
{
  std::shared_lock lock(mutex_);
  for (const auto &executor : executors_) {
    executor->await_block_close(block_id);
  }
}

} // namespace hogd
