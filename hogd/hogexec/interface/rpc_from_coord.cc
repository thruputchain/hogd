#include "hogd/hogexec/interface/rpc_from_coord.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>

#include "exec_result.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/interface/executor_context.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {
namespace {

template<typename T>
x23::Async<ParallelBatch>
to_parallel_result_batch(const x23::XdrVec<T> &results)
{
  ParallelBatch batch;
  co_await t_to_parallel_batch(results, batch);
  co_return batch;
}

} // namespace

x23::Async<SystemStatus<PerThreadARC<ExecutorContext> *>>
ExecutorMempoolRpc::try_get_context(BlockID block_id, ExecutionMode mode)
{
  if (!initialize_check()) [[unlikely]] {
    warn("ExecutorMempoolRpc: accessing context before initialization");
    co_return nullptr;
  }

  auto *ctx = co_await executor_states_->get_executor_context(block_id);
  if (!ctx) {
    co_return nullptr;
  }
  if (ctx->get().mode() != mode) {
    warn("Expected executor context for block {} to be in {} mode, but found "
         "in {} mode",
         block_id, mode, ctx->get().mode());
    if (auto rel = co_await release_context(block_id, ctx); !rel.has_value())
        [[unlikely]] {
      co_return rel.error();
    }
    co_return nullptr;
  }

  co_return ctx;
}

x23::Async<ExecProposeResult>
ExecutorMempoolRpc::executorsched_propose_transaction(
    BlockID block_id, SignedTransaction signed_tx)
{
  auto ctx_status =
      co_await try_get_context(block_id, ExecutionMode::kProposing);
  if (!ctx_status.has_value()) [[unlikely]] {
    co_return ExecProposeResult(ExecResult::ExecResultUnrecoverable);
  }
  auto *ctx = *ctx_status;
  if (!ctx) {
    co_return ExecProposeResult(ExecResult::ExecResultSystemError);
  }

  auto r =
      co_await ctx->get().propose_transaction(signed_tx, allocate_tx_ids(1));
  if (auto rel = co_await release_context(block_id, ctx); !rel.has_value())
      [[unlikely]] {
    co_return ExecProposeResult(ExecResult::ExecResultUnrecoverable);
  }
  co_return r;
}

x23::Async<ExecResult>
ExecutorMempoolRpc::executorsched_validate_transaction(
    BlockID block_id, ProposedTransaction proposed_tx)
{
  auto ctx_status =
      co_await try_get_context(block_id, ExecutionMode::kValidating);
  if (!ctx_status.has_value()) [[unlikely]] {
    co_return ExecResult::ExecResultUnrecoverable;
  }
  auto *ctx = *ctx_status;
  if (!ctx) {
    co_return ExecResult::ExecResultSystemError;
  }

  auto r =
      co_await ctx->get().validate_transaction(proposed_tx, allocate_tx_ids(1));
  if (auto rel = co_await release_context(block_id, ctx); !rel.has_value())
      [[unlikely]] {
    co_return ExecResult::ExecResultUnrecoverable;
  }
  co_return r;
}

x23::Async<ParallelExecProposeResultBatch>
ExecutorMempoolRpc::executorsched_propose_batch(
    BlockID block_id, ParallelSignedTransactionBatch signed_txs_batch)
{
  SignedTransactionBatch signed_txs;
  if (!(co_await parallel_batch_to_t(signed_txs_batch, signed_txs)))
      [[unlikely]] {
    co_return co_await to_parallel_result_batch(ExecProposeBatchResult(
        std::max<size_t>(signed_txs.size(), 1),
        ExecProposeResult(ExecResult::ExecResultUnrecoverable)));
  }

  auto ctx_status =
      co_await try_get_context(block_id, ExecutionMode::kProposing);
  if (!ctx_status.has_value()) [[unlikely]] {
    co_return co_await to_parallel_result_batch(ExecProposeBatchResult(
        signed_txs.size(),
        ExecProposeResult(ExecResult::ExecResultUnrecoverable)));
  }
  auto *ctx = *ctx_status;
  if (!ctx) {
    co_return co_await to_parallel_result_batch(ExecProposeBatchResult(
        signed_txs.size(),
        ExecProposeResult(ExecResult::ExecResultSystemError)));
  }

  CHECK(signed_txs.size() <= std::numeric_limits<uint32_t>::max());
  auto first_tx_id = allocate_tx_ids(signed_txs.size());
  auto r =
      co_await ctx->get().propose_transaction_batch(signed_txs, first_tx_id);
  if (auto rel = co_await release_context(block_id, ctx); !rel.has_value())
      [[unlikely]] {
    co_return co_await to_parallel_result_batch(ExecProposeBatchResult(
        signed_txs.size(),
        ExecProposeResult(ExecResult::ExecResultUnrecoverable)));
  }
  co_return co_await to_parallel_result_batch(r);
}

x23::Async<ParallelExecResultBatch>
ExecutorMempoolRpc::executorsched_validate_batch(
    BlockID block_id, ParallelSignedTransactionBatch signed_txs_batch,
    ParallelNondeterministicProposalDataBatch proposal_data_batch)
{
  SignedTransactionBatch signed_txs;
  x23::XdrVec<NondeterministicProposalData> proposal_data;
  const bool decoded_signed_txs =
      co_await parallel_batch_to_t(signed_txs_batch, signed_txs);
  const bool decoded_proposal_data =
      co_await parallel_batch_to_t(proposal_data_batch, proposal_data);
  if (!decoded_signed_txs || !decoded_proposal_data ||
      signed_txs.size() != proposal_data.size()) [[unlikely]] {
    co_return co_await to_parallel_result_batch(ExecValidateBatchResult(
        std::max<size_t>({signed_txs.size(), proposal_data.size(), 1}),
        ExecResult::ExecResultUnrecoverable));
  }

  auto ctx_status =
      co_await try_get_context(block_id, ExecutionMode::kValidating);
  if (!ctx_status.has_value()) [[unlikely]] {
    co_return co_await to_parallel_result_batch(
        ExecValidateBatchResult(signed_txs.size(),
                                ExecResult::ExecResultUnrecoverable));
  }
  auto *ctx = *ctx_status;
  if (!ctx) {
    co_return co_await to_parallel_result_batch(
        ExecValidateBatchResult(signed_txs.size(),
                                ExecResult::ExecResultSystemError));
  }

  CHECK(signed_txs.size() <= std::numeric_limits<uint32_t>::max());
  auto r = co_await ctx->get().validate_transaction_batch(
      signed_txs, proposal_data, allocate_tx_ids(signed_txs.size()));
  if (auto rel = co_await release_context(block_id, ctx); !rel.has_value())
      [[unlikely]] {
    co_return co_await to_parallel_result_batch(
        ExecValidateBatchResult(signed_txs.size(),
                                ExecResult::ExecResultUnrecoverable));
  }
  co_return co_await to_parallel_result_batch(r);
}

x23::Async<ExecResult>
ExecutorMempoolRpc::executorsched_open_proposal_block(BlockID id)
{
  if (!initialize_check()) [[unlikely]] {
    co_return ExecResult::ExecResultSystemError;
  }
  co_return co_await executor_states_->open_proposal(id);
}

x23::Async<ExecResult>
ExecutorMempoolRpc::executorsched_open_validation_block(BlockID id)
{
  if (!initialize_check()) [[unlikely]] {
    co_return ExecResult::ExecResultSystemError;
  }
  co_return co_await executor_states_->open_validation(id);
}

x23::Async<ExecResult>
ExecutorMempoolRpc::executorsched_finish_block(BlockID id)
{
  if (!initialize_check()) [[unlikely]] {
    co_return ExecResult::ExecResultSystemError;
  }
  co_return co_await executor_states_->remove_block(id);
}

TransactionID
ExecutorMempoolRpc::allocate_tx_ids(uint32_t count)
{
  auto &allocator = *tx_id_allocator_;
  if (allocator.base_pool_ == nullptr) [[unlikely]] {
    allocator.base_pool_ = &base_alloc_pool_;
  }
  uint64_t base = allocator.allocate(count);
  CHECK(base + count <= uint64_t{1} << 48); // another executor's id block
  return TransactionID{base + (uint64_t{self_executor_id_} << 48)};
}

x23::Async<void>
ExecutorMempoolRpc::executorsched_shutdown()
{
  shut_down();
  co_return;
}

x23::MaybeAsync<SystemStatus<void>>
ExecutorMempoolRpc::release_context(BlockID block_id,
                                    PerThreadARC<ExecutorContext> *ctx)
{
  if (ctx->release_reference()) {
    // We dropped the last reference, so the deferred gc runs here. Its drain
    // can surface a late event-server error from a straggler callback; return
    // it so the caller can fold it into the in-flight transaction's result.
    return executor_states_->gc_executor_context(block_id);
  }
  return SystemStatus<void>{};
}

} // namespace hogd
