#pragma once

#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unistd.h>
#include <utility>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/auth/auth_data_cache.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/auth/status.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/transaction_serialization.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "parallel.hh"
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
  explicit AuthServer(dbauth_client_t &&dbauth_client,
                      int bucket_size = 1'048'576)
    : auth_data_cache_(bucket_size), dbauth_client_(std::move(dbauth_client))
  {}

  // The height supplied here should be the height returned by
  // DB::get_dbtip().height. That height is the count of the number of blocks
  // that have been committed.
  x23::Async<AuthErrorCodeBatch>
  verify_transactions(BlockHeight height,
                      std::span<const SignedTransaction> txs,
                      size_t max_parallelism = 1);
  x23::Async<AuthErrorCodeBatch> verify_transactions(
      BlockHeight height, std::span<const SignedTransaction> txs,
      std::span<const x23::Buf> serialized_txs, size_t max_parallelism = 1);
  x23::Async<AuthErrorCode> verify_batch(BlockHeight height,
                                         SignedTransactionBatch txs);
  x23::Async<AuthErrorCode>
  verify_batch(BlockHeight height, SignedTransactionBatch txs,
               SerializedTransactionBatch serialized_txs);

  x23::Async<AuthStatus<AuthCacheEntry>>
  lookup_auth_data(BlockHeight height, ContractAddress address);

  // This mentod can be called in any order.  However, it is vital for cache
  // integrity that batches of block updates are only added to the auth cache in
  // strictly increasing order, with no gaps. The CoroFutex manages this. During
  // initialization from an empty cache, if we get two blocks of updates
  // concurrently, we might process the second before the first.  In this case,
  // the first will be dropped.
  x23::Async<void> register_auth_data_updates(BlockHeight height,
                                              KeyChangeBatch key_changes);

  x23::Async<void> subscribe_to_auth_data_changes(AuthCard card);

  void set_mempool_client(sched_auth_client_t &&client)
  {
    CHECK(!mempool_client_);
    mempool_client_.emplace(std::move(client));
  }

private:
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

  x23::Async<void> authdb_forward_key_changes(BlockHeight height,
                                              KeyChangeBatch key_changes)
  {
    CHECK_PTR(auth_server_);

    return auth_server_->register_auth_data_updates(height,
                                                    std::move(key_changes));
  }

  x23::Async<void> subscribe_to_auth_data_changes(AuthCard card)
  {
    CHECK_PTR(auth_server_);

    return auth_server_->subscribe_to_auth_data_changes(std::move(card));
  }

private:
  AuthServer *auth_server_ = nullptr;
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

  x23::Async<AuthErrorCodeBatch>
  authmempool_verify_transactions(BlockHeight height,
                                  ParallelSignedTransactionBatch batch)
  {
    CHECK_PTR(auth_server_);

    SignedTransactionBatch txs;
    if (!(co_await parallel_batch_to_t(batch, txs))) {
      co_return AuthErrorCodeBatch{};
    }
    SerializedTransactionBatch serialized_txs;
    co_await serialize_transaction_payloads(
        std::span<const SignedTransaction>(txs.begin(), txs.end()),
        serialized_txs);

    // The txs object is valid until the end of the coroutine.
    // Because auth_server_->verify_transactions does not hold onto the txs
    // input after verify_transactions returns, it is safe to pass a span to it.
    co_return co_await auth_server_->verify_transactions(
        height, std::span<const SignedTransaction>(txs.begin(), txs.end()),
        std::span<const x23::Buf>(serialized_txs.begin(), serialized_txs.end()),
        /*max_parallelism=*/available_parallelism());
  }

  x23::Async<AuthErrorCode>
  authmempool_verify_batch(BlockHeight height,
                           ParallelSignedTransactionBatch batch)
  {
    CHECK_PTR(auth_server_);

    SignedTransactionBatch txs;
    if (!(co_await parallel_batch_to_t(batch, txs))) {
      co_return AuthErrorCode::kAuthInternal;
    }
    SerializedTransactionBatch serialized_txs;
    co_await serialize_transaction_payloads(
        std::span<const SignedTransaction>(txs.begin(), txs.end()),
        serialized_txs);
    co_return co_await auth_server_->verify_batch(height, std::move(txs),
                                                  std::move(serialized_txs));
  }

private:
  AuthServer *auth_server_ = nullptr;
};
static_assert(is_AuthMempool1<AuthMempoolRpc *, x23::Async>);

} // namespace hogd
