#pragma once

#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unistd.h>
#include <unordered_map>
#include <utility>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/chunk_position.h"
#include "hogd/base/chunk_run_assembler.h"
#include "hogd/base/chunked_vector.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/hog_auth/auth_data_cache.h"
#include "hogd/hog_auth/dbauth_client.h"
#include "hogd/hog_auth/status.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/rpc.h"

namespace hogd {

using sched_auth_client_t =
    MempoolAuth1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using AuthCacheEntry =
    std::shared_ptr<std::pair<BlockHeight, std::optional<AddressAuthData>>>;

class AuthServer {
public:
  explicit AuthServer(dbauth_client_t &&dbauth_client, NetworkHash network_hash,
                      int bucket_size = 1'048'576)
    : auth_data_cache_(bucket_size), dbauth_client_(std::move(dbauth_client)),
      network_hash_(network_hash)
  {}

  NetworkHash const &network_hash() const { return network_hash_; }

  RPCError open_block(BlockID parent_id, BlockID block_id);
  x23::Async<RPCError> close_block(BlockID block_id);
  x23::Async<RPCError> commit_block(BlockID block_id);
  x23::Async<RPCError> abandon_block(BlockID block_id);

  x23::Async<AuthErrorCodeBatch>
  verify_transactions(BlockID block_id, std::span<const SignedTransaction> txs,
                      size_t max_parallelism = 1);
  x23::Async<AuthErrorCode> verify_batch(BlockID block_id,
                                         SignedTransactionBatch txs);

  // The single-version lookup backend, also used by direct verifier tests.
  // RPC callers must go through a block context above.
  // The height supplied here should be the height returned by
  // DB::get_dbtip().height. That height is the count of the number of blocks
  // that have been committed.
  x23::Async<AuthErrorCodeBatch>
  verify_transactions_at_height(BlockHeight height,
                                std::span<const SignedTransaction> txs,
                                size_t max_parallelism = 1);
  x23::Async<AuthErrorCode> verify_batch_at_height(BlockHeight height,
                                                   SignedTransactionBatch txs);

  x23::Async<AuthStatus<AuthCacheEntry>>
  lookup_auth_data(BlockHeight height, ContractAddress address);

  // Applies a committed block's key changes to the cache, then forwards them
  // to the mempool. [key_changes] is the run of chunks the block arrived in,
  // or a plain batch. This method can be called in any order.  However, it is
  // vital for cache integrity that batches of block updates are only added to
  // the auth cache in strictly increasing order, with no gaps. The CoroFutex
  // manages this. During initialization from an empty cache, if we get two
  // blocks of updates concurrently, we might process the second before the
  // first.  In this case, the first will be dropped.
  x23::Async<void>
  register_auth_data_updates(DBAuthTip tip,
                             ChunkedVector<KeyChangeBatch> key_changes);

  x23::Async<void> subscribe_to_auth_data_changes(AuthCard card);

  void set_mempool_client(sched_auth_client_t &&client)
  {
    CHECK(!mempool_client_);
    mempool_client_.emplace(std::move(client));
  }

private:
  enum class BlockState { kOpen, kClosing, kClosed, kCommitting, kAbandoned };
  struct BlockContext {
    explicit BlockContext(DBAuthTip parent_tip) : parent(parent_tip) {}
    DBAuthTip parent;
    BlockState state = BlockState::kOpen; // guarded by blocks_lock_
    x23::CoroFutex<size_t> active{0};
    // Pending, selected by DB, or rejected by a different committed block.
    x23::CoroFutex<int> decision{0};
  };

  AuthStatus<std::shared_ptr<BlockContext>>
  begin_verification(BlockID block_id);
  static void finish_verification(BlockContext &context);
  bool parent_is_current(const BlockContext &context);
  void publish_tip(DBAuthTip tip);

  std::mutex blocks_lock_;
  std::unordered_map<BlockID, std::shared_ptr<BlockContext>> blocks_;
  std::optional<DBAuthTip> committed_tip_;

  x23::Async<AuthStatus<AuthCacheEntry>>
  populate_auth_data(BlockHeight height, ContractAddress const &address);

  // If the auth data cache contains data for a ContractAddress, then that data
  // must be valid for a block height at least as recent as the current height.
  AuthDataCache auth_data_cache_;
  DBAuthClient dbauth_client_;

  // When initializing from an empty cache: whichever register_auth_data_updates
  // call arrives first will swap the current height to kInitializing.  Any
  // other pending calls will wait before proceeding.
  constexpr static BlockHeight kUninitializedHeight =
      std::numeric_limits<BlockHeight>::max();
  constexpr static BlockHeight kInitializing = kUninitializedHeight - 1;

  // Used to stall incoming requests until the updates from the db are present,
  // and to sequence potentially out of order updates from the db.
  x23::CoroFutex<BlockHeight> current_height_futex_{kUninitializedHeight};

  std::optional<sched_auth_client_t> mempool_client_;

  const NetworkHash network_hash_;
};

class AuthDBRpc {
public:
  using rpc_service = AuthDB1;

  AuthDBRpc() = default;

  void set_auth_server(AuthServer &auth_server)
  {
    CHECK_NULL(auth_server_);
    auth_server_ = &auth_server;
  }

  // Collects the run of chunks hog_db sends for a committed block and hands
  // the auth server the whole height once, so its ordered-height gate keeps
  // seeing one update per height. Chunks may arrive in any order and the runs
  // of several heights may interleave.
  x23::Async<void> authdb_forward_key_changes_chunk(KeyChangeChunk chunk)
  {
    CHECK_PTR(auth_server_);

    std::optional<KeyChangeRunAssembler::Run> run =
        run_assembler_.push({chunk.height, chunk.block_id},
                            ChunkPosition{.index = chunk.chunk_index,
                                          .count = chunk.chunk_count},
                            std::move(chunk.changes));
    if (run.has_value()) {
      co_await auth_server_->register_auth_data_updates(
          DBAuthTip{.block_id = run->key.second, .height = run->key.first},
          std::move(run->parts));
    }
  }

  x23::Async<void> subscribe_to_auth_data_changes(AuthCard card)
  {
    CHECK_PTR(auth_server_);

    return auth_server_->subscribe_to_auth_data_changes(std::move(card));
  }

private:
  using KeyChangeRunAssembler =
      ChunkRunAssembler<std::pair<BlockHeight, BlockID>, KeyChangeBatch>;

  AuthServer *auth_server_ = nullptr;
  KeyChangeRunAssembler run_assembler_;
};

static_assert(is_AuthDB1<AuthDBRpc *, x23::Async>);

class AuthMempoolRpc {
public:
  using rpc_service = AuthMempool1;

  AuthMempoolRpc() = default;

  void set_auth_server(AuthServer &auth_server)
  {
    CHECK_NULL(auth_server_);
    auth_server_ = &auth_server;
  }

  x23::Async<RPCError> authmempool_open_block(BlockID parent_id,
                                              BlockID block_id)
  {
    CHECK_PTR(auth_server_);
    co_return auth_server_->open_block(parent_id, block_id);
  }

  x23::Async<RPCError> authmempool_close_block(BlockID block_id)
  {
    CHECK_PTR(auth_server_);
    return auth_server_->close_block(block_id);
  }

  x23::Async<RPCError> authmempool_commit_block(BlockID block_id)
  {
    CHECK_PTR(auth_server_);
    return auth_server_->commit_block(block_id);
  }

  x23::Async<RPCError> authmempool_abandon_block(BlockID block_id)
  {
    CHECK_PTR(auth_server_);
    return auth_server_->abandon_block(block_id);
  }

  x23::Async<AuthErrorCodeBatch>
  authmempool_verify_transactions(BlockID block_id, SignedTransactionBatch txs)
  {
    CHECK_PTR(auth_server_);

    // The txs object is valid until the end of the coroutine.
    // Because auth_server_->verify_transactions does not hold onto the txs
    // input after verify_transactions returns, it is safe to pass a span to it.
    co_return co_await auth_server_->verify_transactions(
        block_id, std::span<const SignedTransaction>(txs.begin(), txs.end()),
        /*max_parallelism=*/available_parallelism());
  }

  x23::Async<AuthErrorCode> authmempool_verify_batch(BlockID block_id,
                                                     SignedTransactionBatch txs)
  {
    CHECK_PTR(auth_server_);

    return auth_server_->verify_batch(block_id, std::move(txs));
  }

private:
  AuthServer *auth_server_ = nullptr;
};
static_assert(is_AuthMempool1<AuthMempoolRpc *, x23::Async>);

} // namespace hogd
