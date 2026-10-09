#include "hogd/hog_auth/auth_server.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include "auth_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/types.h"
#include "hogd/hog_auth/status.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"

namespace hogd {

RPCError
AuthServer::open_block(BlockID parent_id, BlockID block_id)
{
  std::scoped_lock lock(blocks_lock_);
  if (!committed_tip_ || committed_tip_->block_id != parent_id ||
      committed_tip_->height < 0 ||
      committed_tip_->height >= kInitializing - 1 || block_id == parent_id ||
      blocks_.contains(block_id)) {
    return RPCError::kRPCBadBlockID;
  }
  blocks_.emplace(block_id, std::make_shared<BlockContext>(*committed_tip_));
  return RPCError::kRPCNone;
}

AuthStatus<std::shared_ptr<AuthServer::BlockContext>>
AuthServer::begin_verification(BlockID block_id)
{
  std::scoped_lock lock(blocks_lock_);
  const auto it = blocks_.find(block_id);
  if (it == blocks_.end() || it->second->state != BlockState::kOpen) {
    return AuthError(AuthErrorCode::kAuthBadBlockID);
  }
  auto context = it->second;
  if (!committed_tip_ || committed_tip_->height != context->parent.height ||
      committed_tip_->block_id != context->parent.block_id) {
    return AuthError(AuthErrorCode::kAuthStaleBlockHeight);
  }
  context->active.rmw([](auto &active) { ++active; });
  return context;
}

void
AuthServer::finish_verification(BlockContext &context)
{
  if (context.active.rmw([](auto &active) { return --active; }) == 0) {
    context.active.broadcast(&default_threadpool());
  }
}

bool
AuthServer::parent_is_current(const BlockContext &context)
{
  std::scoped_lock lock(blocks_lock_);
  return committed_tip_ && committed_tip_->height == context.parent.height &&
         committed_tip_->block_id == context.parent.block_id;
}

x23::Async<AuthErrorCodeBatch>
AuthServer::verify_transactions(BlockID block_id,
                                std::span<const SignedTransaction> txs,
                                size_t max_parallelism)
{
  auto context = begin_verification(block_id);
  if (!context) {
    AuthErrorCodeBatch result;
    result.resize(txs.size(), context.error().code());
    co_return result;
  }
  x23::Defer release([&] { finish_verification(**context); });
  auto result =
      co_await verify_transactions_at_height((*context)->parent.height, txs,
                                             max_parallelism);
  if (!parent_is_current(**context)) {
    std::ranges::fill(result, AuthErrorCode::kAuthStaleBlockHeight);
  }
  co_return result;
}

x23::Async<AuthErrorCode>
AuthServer::verify_batch(BlockID block_id, SignedTransactionBatch txs)
{
  auto context = begin_verification(block_id);
  if (!context) {
    co_return context.error().code();
  }
  x23::Defer release([&] { finish_verification(**context); });
  auto result = co_await verify_batch_at_height((*context)->parent.height,
                                                std::move(txs));
  co_return parent_is_current(**context) ? result
                                         : AuthErrorCode::kAuthStaleBlockHeight;
}

x23::Async<RPCError>
AuthServer::close_block(BlockID block_id)
{
  std::shared_ptr<BlockContext> context;
  {
    std::scoped_lock lock(blocks_lock_);
    const auto it = blocks_.find(block_id);
    if (it == blocks_.end() || it->second->state != BlockState::kOpen) {
      co_return RPCError::kRPCBadBlockID;
    }
    context = it->second;
    context->state = BlockState::kClosing;
  }
  while (const size_t active = context->active.load()) {
    co_await context->active.wait(active);
  }
  std::scoped_lock lock(blocks_lock_);
  if (context->state != BlockState::kClosing) {
    co_return RPCError::kRPCBadBlockID;
  }
  context->state = BlockState::kClosed;
  co_return RPCError::kRPCNone;
}

x23::Async<RPCError>
AuthServer::commit_block(BlockID block_id)
{
  std::shared_ptr<BlockContext> context;
  {
    std::scoped_lock lock(blocks_lock_);
    const auto it = blocks_.find(block_id);
    if (it == blocks_.end() || it->second->state != BlockState::kClosed) {
      co_return RPCError::kRPCBadBlockID;
    }
    context = it->second;
    context->state = BlockState::kCommitting;
  }
  int decision;
  while ((decision = context->decision.load()) == 0) {
    co_await context->decision.wait(0);
  }
  {
    std::scoped_lock lock(blocks_lock_);
    blocks_.erase(block_id);
  }
  co_return decision == 1 ? RPCError::kRPCNone : RPCError::kRPCBadBlockID;
}

x23::Async<RPCError>
AuthServer::abandon_block(BlockID block_id)
{
  std::shared_ptr<BlockContext> context;
  {
    std::scoped_lock lock(blocks_lock_);
    const auto it = blocks_.find(block_id);
    if (it == blocks_.end() || it->second->state == BlockState::kCommitting) {
      co_return RPCError::kRPCBadBlockID;
    }
    context = it->second;
    context->state = BlockState::kAbandoned;
    blocks_.erase(it);
  }
  while (const size_t active = context->active.load()) {
    co_await context->active.wait(active);
  }
  co_return RPCError::kRPCNone;
}

void
AuthServer::publish_tip(DBAuthTip tip)
{
  std::scoped_lock lock(blocks_lock_);
  if (committed_tip_ && committed_tip_->height >= tip.height) {
    return;
  }
  committed_tip_ = tip;
  for (auto &[id, context] : blocks_) {
    if (context->parent.height < tip.height && context->decision.load() == 0) {
      context->decision.store(
          id == tip.block_id && context->parent.height + 1 == tip.height ? 1
                                                                         : 2);
      context->decision.broadcast(&default_threadpool());
    }
  }
}

} // namespace hogd
