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
#include "hogd/base/chunk_items.h"
#include "hogd/base/chunked_vector.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_auth/status.h"
#include "hogd/metrics/health.h"
#include "integration.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"
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
    if (current_height < height) {
      co_await current_height_futex_.wait(current_height);
      continue;
    }
    break;
  }
  CHECK_GE(current_height, height);
  CHECK_NE(current_height, kUninitializedHeight);
  CHECK_NE(current_height, kInitializing);

  auto auth_data = auth_data_cache_.get(address);
  if (auth_data != nullptr) {
    // Updates replace an account's cache entry before [current_height_futex_]
    // advances. An entry no newer than the requested height is therefore still
    // valid even when the auth server has since processed newer blocks.
    if (auth_data->first <= height) {
      co_return auth_data;
    }
  }

  if (current_height > height) {
    // [height] is too old for us to look up this particular wallet's auth data.
    co_return AuthError(AuthErrorCode::kAuthStaleBlockHeight);
  }
  CHECK_EQ(current_height, height);

  auto db_auth_data = co_await dbauth_client_.lookup_auth_data(height, address);
  if (!db_auth_data.has_value()) {
    co_return db_auth_data.error();
  }

  auto entry =
      std::make_shared<std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          current_height, std::move(*db_auth_data));

  auth_data_cache_.put(address, entry);

  co_return entry;
}

x23::Async<void>
AuthServer::register_auth_data_updates(
    BlockHeight height, ChunkedVector<KeyChangeBatch> key_changes)
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
    // The mempool's proc still takes the height as one batch.
    std::vector<std::span<const KeyChange>> pieces;
    pieces.reserve(key_changes.chunks().size());
    for (const KeyChangeBatch &chunk : key_changes.chunks()) {
      pieces.emplace_back(chunk.data(), chunk.size());
    }
    co_await mempool_client_->mempoolauth_auth_data_changed(
        height, ChunkItems<KeyChange>(pieces));
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

  CHECK_PTR(entry->get());

  // A missing auth entry (nullopt) is returned as-is: whether that is
  // acceptable depends on the transaction's claim type (pubkey claims
  // require it), so the policy lives in verify_auth.
  co_return std::move(*entry);
}

namespace {

// Auth is the one place the whole SignedTransaction is measured; the executor
// caps only the body (#3844).
bool
wrapper_over_cap(const SignedTransaction &tx)
{
  const size_t wrapper_size = x23::xdr_size(tx);
  if (wrapper_size <= kMaxBytesSignedTransaction) {
    return false;
  }
  trace("auth: rejecting over-cap transaction wrapper ({} bytes, cap {})",
        wrapper_size, kMaxBytesSignedTransaction);
  return true;
}

} // namespace

x23::Async<AuthErrorCodeBatch>
AuthServer::verify_transactions(BlockHeight height,
                                std::span<const SignedTransaction> txs,
                                size_t max_parallelism)
{
  CHECK_GT(max_parallelism, 0UL);
  if (txs.empty()) {
    co_return AuthErrorCodeBatch{};
  }
  co_await default_coropoll().pool_.bg();

  AuthErrorCodeBatch results;
  results.resize(txs.size(), AuthErrorCode::kAuthSuccess);

  std::unordered_map<ContractAddress, AuthCacheEntry, SipHash24Hasher>
      auth_data;
  // The map owns each snapshot until all workers finish. These pointers avoid
  // hashing the address again in every worker, including repeated addresses.
  std::vector<const AddressAuthData *> tx_auth_data(txs.size());
  for (size_t i = 0; i < txs.size(); i++) {
    const SignedTransaction &tx = txs[i];
    if (wrapper_over_cap(tx)) {
      results[i] = AuthErrorCode::kAuthFailed;
      continue;
    }
    auto [it, inserted] =
        auth_data.try_emplace(tx.tx.invocation.invoked_address);
    if (inserted) {
      auto entry =
          co_await populate_auth_data(height, tx.tx.invocation.invoked_address);
      if (!entry.has_value()) {
        trace("Populate auth data error: {}",
              std::to_underlying(entry.error().code()));
        results[i] = entry.error().code();
        auth_data.erase(it);
        continue;
      }
      it->second = std::move(*entry);
    }
    const auto &source_auth_data = it->second->second;
    tx_auth_data[i] = source_auth_data ? &*source_auth_data : nullptr;
  }

  auto verify_chunk = [&](ChunkRange chunk) {
    for (size_t i = chunk.start; i < chunk.end; i++) {
      if (results[i] != AuthErrorCode::kAuthSuccess) {
        continue;
      }
      if (!verify_auth(txs[i], tx_auth_data[i], network_hash_)) {
        results[i] = AuthErrorCode::kAuthFailed;
      }
    }
  };

  if (max_parallelism == 1 || txs.size() == 1) {
    verify_chunk(ChunkRange{.start = 0, .end = txs.size()});
    co_return results;
  }

  const std::vector<ChunkRange> chunks =
      make_chunks(txs.size(), max_parallelism);
  std::vector<x23::Async<void>> handles;
  handles.reserve(chunks.size() - 1);
  for (size_t i = 1; i < chunks.size(); ++i) {
    handles.push_back(
        [](ChunkRange chunk, const auto &verify) noexcept -> x23::Async<void> {
          co_await default_coropoll().pool_.bg();
          verify(chunk);
          co_return;
        }(chunks[i], verify_chunk));
  }
  // Preparation already runs on the pool; use its worker for the first chunk.
  verify_chunk(chunks.front());
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
    if (wrapper_over_cap(tx)) {
      co_return AuthErrorCode::kAuthFailed;
    }
    auto source_addr =
        co_await lookup_auth_data(height, tx.tx.invocation.invoked_address);
    if (!source_addr.has_value()) {
      co_return source_addr.error().code();
    }
    CHECK_PTR(source_addr->get());
    const std::optional<AddressAuthData> &source_auth_data =
        (*source_addr)->second;

    auto verify_result =
        verify_auth(tx, source_auth_data ? &*source_auth_data : nullptr,
                    network_hash_);
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
