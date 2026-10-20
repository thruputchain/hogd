#include "hogd/hog_auth/auth_server.h"

#include <memory>
#include <optional>
#include <span>
#include <tuple>
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

namespace {

bool
same_auth_rules(const AddressAuthData *before, const AddressAuthData *after)
{
  // Review this comparison when the protocol adds auth fields. Only the write
  // height is metadata; row presence, every signing key and its weight, and
  // the required weight all affect verification.
  static_assert(std::tuple_size_v<AddressAuthData> == 3);
  if (before == nullptr || after == nullptr) {
    return before == after;
  }
  return before->public_keys == after->public_keys &&
         before->required_weight == after->required_weight;
}

} // namespace

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

  std::vector<std::span<const KeyChange>> pieces;
  pieces.reserve(key_changes.chunks().size());
  for (const KeyChangeBatch &chunk : key_changes.chunks()) {
    size_t first = 0;
    for (size_t i = 0; i < chunk.size(); ++i) {
      const KeyChange &key_change = chunk[i];
      // A normal execution can rewrite an auth row only to stamp its height.
      // Keep that full cache update, but avoid invalidating pending mempool
      // transactions when the signing rules are known to be unchanged. A
      // cache miss or an entry already at this height is not proof of the
      // previous rules, so conservatively forward those updates.
      const auto previous =
          mempool_client_ ? auth_data_cache_.get(key_change.address) : nullptr;
      const bool unchanged =
          previous && previous->first < height &&
          same_auth_rules(previous->second ? &*previous->second : nullptr,
                          key_change.auth_data.get());
      auto entry = std::make_shared<
          std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          height, key_change.auth_data
                      ? std::optional<AddressAuthData>(*key_change.auth_data)
                      : std::nullopt);
      auth_data_cache_.put(key_change.address, std::move(entry));
      if (unchanged) {
        if (first < i) {
          pieces.emplace_back(chunk.data() + first, i - first);
        }
        first = i + 1;
      }
    }
    if (first < chunk.size()) {
      pieces.emplace_back(chunk.data() + first, chunk.size() - first);
    }
  }
  current_height_futex_.store(height);
  current_height_futex_.broadcast(&default_threadpool());
  co_await Health::instance().set_current_block_height(height);

  if (mempool_client_) {
    // Forward every height, even when all row rewrites were metadata-only:
    // admissions wait for the mempool's auth-data watermark to advance.
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
  co_await default_coropoll().pool_.bg();

  AuthErrorCodeBatch results;
  results.resize(txs.size(), AuthErrorCode::kAuthSuccess);

  std::unordered_map<ContractAddress, AuthCacheEntry, SipHash24Hasher>
      auth_data;
  for (size_t i = 0; i < txs.size(); i++) {
    const SignedTransaction &tx = txs[i];
    if (wrapper_over_cap(tx)) {
      results[i] = AuthErrorCode::kAuthFailed;
      continue;
    }
    if (!auth_data.contains(tx.tx.invocation.invoked_address)) {
      auto entry =
          co_await populate_auth_data(height, tx.tx.invocation.invoked_address);
      if (!entry.has_value()) {
        trace("Populate auth data error: {}",
              std::to_underlying(entry.error().code()));
        results[i] = entry.error().code();
        continue;
      }
      auth_data[tx.tx.invocation.invoked_address] = std::move(*entry);
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
           NetworkHash network_hash,
           AuthErrorCodeBatch &results) noexcept -> x23::Async<void> {
          co_await default_coropoll().pool_.bg();

          x23::Buf signature_preimage;
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
                verify_auth(tx, source_auth_data ? &*source_auth_data : nullptr,
                            network_hash, signature_preimage);
            if (!verify_result.has_value()) {
              results[i] = AuthErrorCode::kAuthFailed;
              continue;
            }
            // [results] is initialized with all of its members as
            // [AuthErrorCode::kAuthSuccess], so there is no need for us to
            // write this result to [results] again.
          }
          co_return;
        }(chunk, txs, auth_data, network_hash_, results));
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
  x23::Buf signature_preimage;
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
                    network_hash_, signature_preimage);
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
