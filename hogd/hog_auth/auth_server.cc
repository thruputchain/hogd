#include "hogd/hog_auth/auth_server.h"

#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/auth_claim/status.h"
#include "hogd/auth_claim/verify.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_auth/status.h"
#include "hogd/metrics/health.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {

x23::Async<AuthStatus<AuthCacheEntry>>
AuthServer::lookup_auth_data(BlockHeight height, ContractAddress address)
{
  CHECK_NE(height, kUninitializedHeight);
  CHECK_NE(height, kInitializing);
  BlockHeight current_height;
  for (;;) {
    current_height = current_height_futex_.load();
    if (current_height == kUninitializedHeight ||
        current_height == kInitializing) {
      co_await current_height_futex_.wait(current_height);
      continue;
    }
    if (current_height > height) {
      co_return AuthError(AuthErrorCode::kAuthStaleBlockHeight);
    }
    if (current_height < height) {
      co_await current_height_futex_.wait(current_height);
      continue;
    }
    break;
  }
  CHECK_EQ(current_height, height);
  CHECK_NE(current_height, kUninitializedHeight);
  CHECK_NE(current_height, kInitializing);

  auto auth_data = auth_data_cache_.get(address);
  if (auth_data != nullptr) {
    // Auth data cache has processed all updates from some initial height up to
    // and including current_height. That means that if the data is present
    // there, and it's not from a block in the future, then it must be valid for
    // current_height.
    if (auth_data->first <= current_height) {
      co_return auth_data;
    }
  }

  auto db_auth_data = co_await dbauth_client_.lookup_auth_data(height, address);
  if (!db_auth_data.has_value()) {
    co_return db_auth_data.error();
  }

  auto entry =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          current_height, db_auth_data.value());

  auth_data_cache_.put(address, entry);

  co_return entry;
}

x23::Async<void>
AuthServer::register_auth_data_updates(BlockHeight height,
                                       KeyChangeBatch key_changes)
{
  // TODO(geoff): this logic will break if there's a sharded db, and i.e.
  // multiple batches of updates from the same block height. Probably the
  // simplest solution will be to have a separate coro futex and separate auth
  // cache for each one.
  for (;;) {
    BlockHeight current_height = current_height_futex_.load();
    if (current_height == kUninitializedHeight) {
      bool acquired_lock = current_height_futex_.rmw([&](auto &value) {
        if (value == kUninitializedHeight) {
          value = kInitializing;
          return true;
        }
        return false;
      });
      if (acquired_lock) {
        break;
      }
      co_await current_height_futex_.wait(current_height);
      continue;
    }

    if (current_height == kInitializing) {
      co_await current_height_futex_.wait(current_height);
      continue;
    }

    if (current_height + 1 == height) {
      break;
    }

    if (current_height >= height) {
      warn("AuthData Cache: Received stale update for block height {} at "
           "current height {}, ignoring.",
           height, current_height);
      co_return;
    }

    co_await current_height_futex_.wait(current_height);
  }

  for (const auto &key_change : key_changes) {
    if (key_change.auth_data == nullptr) {
      auto entry = std::make_shared<
          std::pair<BlockHeight, std::optional<AddressAuthData>>>(height,
                                                                  std::nullopt);
      auth_data_cache_.put(key_change.address, entry);
      continue;
    }
    auto entry = std::make_shared<
        std::pair<BlockHeight, std::optional<AddressAuthData>>>(
        height, *key_change.auth_data);
    auth_data_cache_.put(key_change.address, entry);
  }
  current_height_futex_.store(height);
  current_height_futex_.broadcast(&default_threadpool());
  co_await Health::instance().set_current_block_height(height);

  if (mempool_client_) {
    co_await mempool_client_->mempoolauth_auth_data_changed(height,
                                                            key_changes);
  }
}

x23::Async<void>
// NOLINTNEXTLINE(performance-unnecessary-value-param)
AuthServer::subscribe_to_auth_data_changes(AuthCard card)
{
  BlockHeight current_height =
      co_await dbauth_client_.subscribe_to_auth_data_changes(card);
  if (current_height != -1) {
    bool initialized = current_height_futex_.rmw([&](auto &value) {
      if (value == kUninitializedHeight) {
        value = current_height;
        return true;
      }
      return false;
    });
    if (initialized) {
      info("initializing auth server to height {}", current_height);
      current_height_futex_.broadcast(&default_threadpool());
      co_await Health::instance().set_current_block_height(current_height);
      // No update callback is delivered for blocks that committed before this
      // subscription was installed.  Tell the mempool which height the auth
      // server initialized from so its downstream watermark cannot remain
      // behind forever.  An empty batch is sufficient: a freshly initialized
      // mempool has no cached transactions that need revalidation.
      if (mempool_client_) {
        co_await mempool_client_->mempoolauth_auth_data_changed(
            current_height, KeyChangeBatch{});
      }
    }
    else {
      info("auth server already initialized to height {} by concurrent "
           "update, intro height was {}",
           current_height_futex_.load(), current_height);
    }
  }
  else {
    info("not initializing auth server height");
  }
  co_return;
}

x23::Async<AuthStatus<AuthCacheEntry>>
AuthServer::populate_auth_data(BlockHeight height,
                               ContractAddress const &address)
{
  auto entry = co_await lookup_auth_data(height, address);
  if (!entry.has_value()) {
    co_return entry.error();
  }

  CHECK_PTR(entry.value().get());

  // A missing auth entry (nullopt) is returned as-is: whether that is
  // acceptable depends on the transaction's claim type (pubkey claims
  // require it), so the policy lives in verify_auth.
  co_return std::move(entry.value());
}

x23::Async<AuthErrorCodeBatch>
AuthServer::verify_transactions(BlockHeight height,
                                std::span<const SignedTransaction> txs,
                                size_t max_parallelism)
{
  CHECK_GT(max_parallelism, 0UL);
  co_await default_coropoll().pool_.bg();

  AuthErrorCodeBatch results;
  results.resize(txs.size(), AuthErrorCode::kAuthSuccess);

  std::unordered_map<ContractAddress, AuthCacheEntry, SipHash24Hasher>
      auth_data;
  for (size_t i = 0; i < txs.size(); i++) {
    const SignedTransaction &tx = txs[i];
    if (!auth_data.contains(tx.tx.invocation.invoked_address)) {
      auto entry =
          co_await populate_auth_data(height, tx.tx.invocation.invoked_address);
      if (!entry.has_value()) {
        trace("Populate auth data error: {}",
              std::to_underlying(entry.error().code()));
        results[i] = entry.error().code();
        continue;
      }
      auth_data[tx.tx.invocation.invoked_address] = std::move(entry.value());
    }
  }

  const std::vector<ChunkRange> chunks =
      make_chunks(txs.size(), max_parallelism);
  std::vector<x23::Async<void>> handles;
  handles.reserve(chunks.size());
  for (const ChunkRange &chunk : chunks) {
    handles.push_back(
        [](ChunkRange chunk, x23::span<const SignedTransaction> txs,
           const std::unordered_map<ContractAddress, AuthCacheEntry,
                                    SipHash24Hasher> &auth_data,
           AuthErrorCodeBatch &results) -> x23::Async<void> {
          co_await default_coropoll().pool_.bg();

          for (size_t i = chunk.start; i < chunk.end; i++) {
            const SignedTransaction &tx = txs[i];
            if (results[i] != AuthErrorCode::kAuthSuccess) {
              continue;
            }

            const ContractAddress &account = tx.tx.invocation.invoked_address;
            const auto account_it = auth_data.find(account);
            CHECK(account_it != auth_data.end());
            const std::optional<AddressAuthData> &source_auth_data =
                account_it->second->second;

            AuthVerifyStatus<void> verify_result =
                verify_auth(tx,
                            source_auth_data ? &*source_auth_data : nullptr);
            if (!verify_result.has_value()) {
              results[i] = AuthErrorCode::kAuthFailed;
              continue;
            }
            // [results] is initialized with all of its members as
            // [AuthErrorCode::kAuthSuccess], so there is no need for us to
            // write this result to [results] again.
          }
          co_return;
        }(chunk, txs, auth_data, results));
  }
  for (x23::Async<void> &handle : handles) {
    co_await handle;
  }
  co_return results;
}

x23::Async<AuthErrorCode>
AuthServer::verify_batch(BlockHeight height, SignedTransactionBatch txs)
{
  Ed25519SignatureVerifier signature_verifier;
  for (const auto &tx : txs) {
    auto source_addr =
        co_await lookup_auth_data(height, tx.tx.invocation.invoked_address);
    if (!source_addr.has_value()) {
      co_return source_addr.error().code();
    }
    CHECK_PTR(source_addr.value().get());
    const std::optional<AddressAuthData> &source_auth_data =
        source_addr.value()->second;

    auto verify_result =
        verify_auth(tx, source_auth_data ? &*source_auth_data : nullptr);
    if (!verify_result.has_value()) {
      co_return AuthErrorCode::kAuthFailed;
    }
  }
  if (signature_verifier.verify()) {
    co_return AuthErrorCode::kAuthSuccess;
  }
  trace("Signature verification failed");
  co_return AuthErrorCode::kAuthFailed;
}
} // namespace hogd
