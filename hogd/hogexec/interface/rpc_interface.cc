#include "hogd/hogexec/interface/rpc_interface.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

uint16_t
ExecutorRpcInterface::start_server(uint16_t port_number)
{
  x23::UniqueFd lfd = x23::tcplisten(port_number);
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  // If the [port_number] parameter is 0, then the kernel chooses the port
  // number for us. Thus, in that case, [port_number] will be assigned a nonzero
  // value below.
  port_number = x23::get_port(x23::local_addr(lfd.get()));

  [](x23::CoroPoll &cp, ExecutorRpcInterface *server,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await cp.pool_.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c).server(server);
    }
  }(cp_, this, std::move(lfd));

  return port_number;
}

x23::Async<ExecProposeResult>
ExecutorRpcInterface::executorcoord_propose_transaction(
    BlockID block_id, SignedTransaction signed_tx)
{
  auto ctx = executor_states_.get_executor_context(block_id,
                                                   ExecutionMode::kProposing);
  if (!ctx) {
    co_return ExecProposeResult(ExecResult::ExecResultSystemError);
  }

  co_return co_await ctx->propose_transaction(signed_tx, allocate_tx_id());
}

x23::Async<ExecResult>
ExecutorRpcInterface::executorcoord_validate_transaction(
    BlockID block_id, ProposedTransaction proposed_tx)
{
  auto ctx = executor_states_.get_executor_context(block_id,
                                                   ExecutionMode::kValidating);
  if (!ctx) {
    co_return ExecResult::ExecResultSystemError;
  }

  co_return co_await ctx->validate_transaction(proposed_tx, allocate_tx_id());
}

x23::Async<ExecProposeBatchResult>
ExecutorRpcInterface::executorcoord_propose_batch(
    BlockID block_id, SignedTransactionBatch signed_txs)
{
  auto ctx = executor_states_.get_executor_context(block_id,
                                                   ExecutionMode::kProposing);
  if (!ctx) {
    co_return ExecProposeBatchResult(signed_txs.size(),
                                     ExecProposeResult(
                                         ExecResult::ExecResultSystemError));
  }

  x23::Vec<ExecProposeResult> results;
  results.reserve(signed_txs.size());
  for (SignedTransaction const &signed_tx : signed_txs) {
    results.push_back(
        co_await ctx->propose_transaction(signed_tx, allocate_tx_id()));
  }

  co_return results;
}

x23::Async<ExecValidateBatchResult>
ExecutorRpcInterface::executorcoord_validate_batch(
    BlockID block_id, ProposedTransactionBatch proposed_txs)
{
  auto ctx = executor_states_.get_executor_context(block_id,
                                                   ExecutionMode::kValidating);
  if (!ctx) {
    co_return ExecValidateBatchResult(proposed_txs.size(),
                                      ExecResult::ExecResultSystemError);
  }

  x23::Vec<ExecResult> results;
  results.reserve(proposed_txs.size());
  for (auto const &proposed_tx : proposed_txs) {
    results.push_back(
        co_await ctx->validate_transaction(proposed_tx, allocate_tx_id()));
  }

  co_return results;
}

x23::Async<ExecResult>
ExecutorRpcInterface::executorcoord_open_proposal_block(BlockID id)
{
  executor_states_.open_proposal(id, main_worker_pool_);
  co_return ExecResult::ExecResultOK;
}

x23::Async<ExecResult>
ExecutorRpcInterface::executorcoord_open_validation_block(BlockID id)
{
  executor_states_.open_validation(id, main_worker_pool_);
  co_return ExecResult::ExecResultOK;
}

x23::Async<void>
ExecutorRpcInterface::executorcoord_finish_block(BlockID id)
{
  executor_states_.remove_block(id);
  co_return;
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
