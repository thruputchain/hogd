#include "hogd/hogexec/interface/rpc_interface.h"

#include <atomic>
#include <cassert>
#include <cstdlib>

#include "exec_sched.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/vec.h"

namespace hogd {

uint16_t
ExecutorRpcInterface::start_server(uint16_t port_number)
{
  port_ = tcp_rpc_server(cp_, port_number, [this](auto c) { c->server(this); });
  return port_;
}

x23::Async<ExecProposeResult>
ExecutorRpcInterface::executorcoord_propose_transaction(
    BlockID block_id, SignedTransaction signed_tx)
{
  // TODO(geoff): co_await cp_.pool_.bg();
  auto ctx = executor_states_->get_executor_context(block_id);
  if (!ctx) {
    co_return ExecProposeResult(ExecResult::ExecResultSystemError);
  }
  if (ctx->mode() != ExecutionMode::kProposing) {
    co_return ExecProposeResult(ExecResult::ExecResultSystemError);
  }

  co_return ctx->propose_transaction(signed_tx, allocate_tx_id());
}

x23::Async<ExecResult>
ExecutorRpcInterface::executorcoord_validate_transaction(
    BlockID block_id, ProposedTransaction proposed_tx)
{
  // TODO(geoff): co_await cp_.pool_.bg();
  auto ctx = executor_states_->get_executor_context(block_id);
  if (!ctx) {
    co_return ExecResult::ExecResultSystemError;
  }
  if (ctx->mode() != ExecutionMode::kValidating) {
    co_return ExecResult::ExecResultSystemError;
  }

  co_return ctx->validate_transaction(proposed_tx, allocate_tx_id());
}

x23::Async<ExecProposeBatchResult>
ExecutorRpcInterface::executorcoord_propose_batch(
    BlockID block_id, SignedTransactionBatch signed_txs)
{
  // TODO(geoff): co_await cp_.pool_.bg();
  co_await cp_.pool_.bg();
  auto ctx = executor_states_->get_executor_context(block_id);
  if (!ctx) {
    co_return ExecProposeBatchResult(signed_txs.size(),
                                     ExecProposeResult(
                                         ExecResult::ExecResultSystemError));
  }
  if (ctx->mode() != ExecutionMode::kProposing) {
    co_return ExecProposeBatchResult(signed_txs.size(),
                                     ExecProposeResult(
                                         ExecResult::ExecResultSystemError));
  }

  x23::Vec<ExecProposeResult> results;
  results.reserve(signed_txs.size());
  for (SignedTransaction const &signed_tx : signed_txs) {
    results.push_back(ctx->propose_transaction(signed_tx, allocate_tx_id()));
  }

  co_return results;
}

x23::Async<ExecValidateBatchResult>
ExecutorRpcInterface::executorcoord_validate_batch(
    BlockID block_id, ProposedTransactionBatch proposed_txs)
{
  // TODO(geoff): co_await cp_.pool_.bg();
  auto ctx = executor_states_->get_executor_context(block_id);
  if (!ctx) {
    co_return ExecValidateBatchResult(proposed_txs.size(),
                                      ExecResult::ExecResultSystemError);
  }
  if (ctx->mode() != ExecutionMode::kValidating) {
    co_return ExecValidateBatchResult(proposed_txs.size(),
                                      ExecResult::ExecResultSystemError);
  }

  x23::Vec<ExecResult> results;
  results.reserve(proposed_txs.size());
  for (auto const &proposed_tx : proposed_txs) {
    results.push_back(ctx->validate_transaction(proposed_tx, allocate_tx_id()));
  }

  co_return results;
}

ExecResult
ExecutorRpcInterface::executorcoord_open_proposal_block(BlockID id)
{
  return executor_states_->open_proposal(id);
}

ExecResult
ExecutorRpcInterface::executorcoord_open_validation_block(BlockID id)
{
  return executor_states_->open_validation(id);
}

void
ExecutorRpcInterface::executorcoord_finish_block(BlockID id)
{
  executor_states_->remove_block(id);
}

TransactionID
ExecutorRpcInterface::allocate_tx_id()
{
  uint64_t base = self_id_.fetch_add(1, std::memory_order_relaxed);
  return TransactionID{base + (uint64_t{self_executor_id_} << 48)};
}

void
ExecutorRpcInterface::shut_down()
{
  shutdown_flag_ = true;
}

x23::Async<void>
ExecutorRpcInterface::executorcoord_shutdown()
{
  shut_down();
  co_return;
}

} // namespace hogd
