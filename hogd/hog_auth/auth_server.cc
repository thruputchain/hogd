#include "hogd/hog_auth/auth_server.h"

#include <algorithm>
#include <array>
#include <exception>
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
#include "hogd/base/chunk_position.h"
#include "hogd/base/chunk_run_sender.h"
#include "hogd/base/chunked_vector.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/key_change_chunk_view.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_auth/dbauth_client.h"
#include "hogd/hog_auth/status.h"
#include "hogd/metrics/health.h"
#include "integration.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

AuthCacheEntry
AuthServer::cached_entry_at(BlockHeight height, const ContractAddress &address)
{
  AuthCacheEntry auth_data = auth_data_cache_.get(address);
  // Updates replace an account's cache entry before [current_height_futex_]
  // advances. An entry no newer than the requested height is therefore still
  // valid even when the auth server has since processed newer blocks.
  if (auth_data != nullptr && auth_data->first <= height) {
    return auth_data;
  }
  return nullptr;
}

AuthCacheEntry
AuthServer::try_cached_auth_data(BlockHeight height,
                                 const ContractAddress &address)
{
  CHECK_NE(height, kUninitializedHeight);
  CHECK_NE(height, kInitializing);
  // The checks lookup_auth_data() waits on, without waiting.
  const BlockHeight current_height = current_height_futex_.load();
  if (current_height == kUninitializedHeight ||
      current_height == kInitializing || current_height < height) {
    return nullptr;
  }
  return cached_entry_at(height, address);
}

x23::Async<BlockHeight>
AuthServer::wait_for_height(BlockHeight height)
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
  co_return current_height;
}

AuthStatus<AuthCacheEntry>
AuthServer::check_auth_cache(BlockHeight height, BlockHeight current_height,
                             const ContractAddress &address)
{
  CHECK_GE(current_height, height);
  if (AuthCacheEntry entry = cached_entry_at(height, address)) {
    return entry;
  }

  if (current_height > height) {
    // [height] is too old for us to look up this particular wallet's auth data.
    return AuthError(AuthErrorCode::kAuthStaleBlockHeight);
  }
  return AuthCacheEntry{};
}

x23::Async<AuthStatus<AuthCacheEntry>>
AuthServer::lookup_auth_data(BlockHeight height, ContractAddress address)
{
  const BlockHeight current_height = co_await wait_for_height(height);
  auto cached = check_auth_cache(height, current_height, address);
  if (!cached.has_value() || *cached != nullptr) {
    co_return cached;
  }

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

  co_await forward_key_changes(height, key_changes);
}

x23::Async<void>
AuthServer::ForwardKeyChangeChunk::operator()(ChunkPosition position,
                                              ChunkItems<KeyChange> changes)
{
  // The record is serialized from [changes] in place, before the call first
  // suspends.
  const KeyChangeChunkView view{.height = height,
                                .chunk_index = position.index,
                                .chunk_count = position.count,
                                .changes = changes};
  co_await client.mempoolauth_auth_data_changed_chunk(view);
}

x23::Async<void>
AuthServer::forward_key_changes(
    BlockHeight height, const ChunkedVector<KeyChangeBatch> &key_changes)
{
  if (!mempool_client_) {
    co_return;
  }
  // The chunks are serialized from [key_changes] in place; it outlives the
  // send.
  ChunkRunSender sender(key_changes.chunks(), max_key_changes_per_chunk_,
                        ForwardKeyChangeChunk{.client = *mempool_client_,
                                              .height = height});
  co_await sender.send();
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

x23::Async<IntroAuthResult>
AuthServer::subscribe_and_register(AuthCard card)
{
  if (!mempool_client_) {
    co_return IntroAuthResult(IntroAuthOutcome::kFailure);
  }
  // [set_mempool_client] sets the client once and nothing resets it, so the
  // reference outlives the subscription's suspension.
  sched_auth_client_t &mempool_client = *mempool_client_;
  co_await subscribe_to_auth_data_changes(card);
  co_return co_await mempool_client.mempoolauth_intro_auth(std::move(card));
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

using AuthDataByAccount =
    std::unordered_map<ContractAddress, AuthCacheEntry, SipHash24Hasher>;

// Verifies each of [txs] against its account's entry in [auth_data], writing
// kAuthFailed to the matching element of [results] for each that fails. Skips
// transactions the lookup pass already failed (over the size cap, or no auth
// data); they have no entry in [auth_data].
void
verify_chunk(std::span<const SignedTransaction> txs,
             std::span<AuthErrorCode> results,
             const AuthDataByAccount &auth_data,
             const NetworkHash &network_hash)
{
  x23::Buf signature_preimage;
  for (size_t i = 0; i < txs.size(); i++) {
    const SignedTransaction &tx = txs[i];
    if (results[i] != AuthErrorCode::kAuthSuccess) {
      continue;
    }

    const auto account_it = auth_data.find(tx.tx.invocation.invoked_address);
    CHECK(account_it != auth_data.end());
    const std::optional<AddressAuthData> &source_auth_data =
        account_it->second->second;

    AuthVerifyStatus<void> verify_result =
        verify_auth(tx, source_auth_data ? &*source_auth_data : nullptr,
                    network_hash, signature_preimage);
    if (!verify_result.has_value()) {
      results[i] = AuthErrorCode::kAuthFailed;
    }
    // [results] starts as all kAuthSuccess, so a pass needs no write.
  }
}

// verify_chunk() on the pool, for one chunk of a call split over several.
x23::Async<void>
verify_chunk_on_pool(std::span<const SignedTransaction> txs,
                     std::span<AuthErrorCode> results,
                     const AuthDataByAccount &auth_data,
                     NetworkHash network_hash)
{
  co_await default_coropoll().pool_.bg();
  verify_chunk(txs, results, auth_data, network_hash);
}

} // namespace

x23::Async<void>
AuthServer::gather_missing_auth_data(BlockHeight height,
                                     std::span<const SignedTransaction> txs,
                                     size_t first_missing,
                                     AuthDataByAccount &auth_data,
                                     AuthErrorCodeBatch &results)
{
  std::unordered_map<ContractAddress, AuthStatus<AuthCacheEntry>,
                     SipHash24Hasher>
      missing_auth_data;
  ContractAddressBatch missing_addresses;
  std::vector<AuthStatus<AuthCacheEntry> *> missing_entries;
  // Reuse a fixed set of slots to bound outstanding RPCs without allocating
  // a queue on the warm-cache path.
  std::array<std::optional<x23::Async<void>>, kMaxLookupBatchesInFlight>
      pending_lookups;
  size_t next_lookup = 0;

  // Each coroutine owns its arguments and writes only its distinct missing
  // map entries. Rehashing preserves their addresses; verification starts
  // after every lookup has finished.
  auto fetch_missing = [&](ContractAddressBatch addresses,
                           std::vector<AuthStatus<AuthCacheEntry> *> entries)
      -> x23::Async<void> {
    std::vector<AuthStatus<std::optional<AddressAuthData>>> replies;
    try {
      replies =
          co_await dbauth_client_.lookup_auth_data_batch(height, addresses);
    } catch (const std::exception &e) {
      // Convert a failed RPC to per-address outcomes so the other in-flight
      // lookups are still joined before their destination entries are freed.
      warn("DB auth lookup batch failed: {}", e.what());
      for (auto *entry : entries) {
        *entry = AuthError(AuthErrorCode::kAuthInternal);
      }
      co_return;
    }
    for (size_t i = 0; i < replies.size(); ++i) {
      if (!replies[i].has_value()) {
        *entries[i] = replies[i].error();
        continue;
      }
      auto entry = std::make_shared<
          std::pair<BlockHeight, std::optional<AddressAuthData>>>(
          height, std::move(*replies[i]));
      // put() rejects older replies if an update arrived while the RPC ran.
      // This request still owns its snapshot even if the cache rejects it.
      auth_data_cache_.put(addresses[i], entry);
      *entries[i] = std::move(entry);
    }
  };

  auto launch_lookup = [&]() -> x23::Async<void> {
    if (missing_addresses.empty()) {
      co_return;
    }
    auto &slot = pending_lookups[next_lookup];
    if (slot.has_value()) {
      co_await *slot;
      slot.reset();
    }
    slot.emplace(fetch_missing(std::exchange(missing_addresses, {}),
                               std::exchange(missing_entries, {})));
    next_lookup = (next_lookup + 1) % pending_lookups.size();
  };

  for (size_t i = first_missing; i < txs.size(); i++) {
    const SignedTransaction &tx = txs[i];
    if (wrapper_over_cap(tx)) {
      results[i] = AuthErrorCode::kAuthFailed;
      continue;
    }
    const ContractAddress &account = tx.tx.invocation.invoked_address;
    if (auth_data.contains(account)) {
      continue;
    }
    if (!missing_auth_data.empty() && missing_auth_data.contains(account)) {
      continue;
    }
    if (AuthCacheEntry entry = try_cached_auth_data(height, account)) {
      auth_data.emplace(account, std::move(entry));
      continue;
    }
    auto [it, inserted] = missing_auth_data.try_emplace(account);
    CHECK(inserted);
    const BlockHeight current_height = co_await wait_for_height(height);
    it->second = check_auth_cache(height, current_height, account);
    if (it->second.has_value() && *it->second == nullptr) {
      missing_addresses.push_back(account);
      missing_entries.push_back(&it->second);
      if (missing_addresses.size() == dbauth_client_.max_lookup_batch_size()) {
        co_await launch_lookup();
      }
    }
  }
  if (!missing_addresses.empty()) {
    co_await launch_lookup();
  }
  for (auto &lookup : pending_lookups) {
    if (lookup.has_value()) {
      co_await *lookup;
    }
  }

  if (!missing_auth_data.empty()) {
    // Join all outcomes before publishing fetched snapshots to the positive
    // map. Errors belong only to this request, never the cache. Leave mixed
    // result positions failed so the original verifier skips them.
    for (auto &[account, outcome] : missing_auth_data) {
      if (outcome.has_value()) {
        CHECK_PTR(outcome->get());
        auth_data.emplace(account, std::move(*outcome));
      }
    }
    for (size_t i = first_missing; i < txs.size(); ++i) {
      if (results[i] != AuthErrorCode::kAuthSuccess) {
        continue;
      }
      const auto miss =
          missing_auth_data.find(txs[i].tx.invocation.invoked_address);
      if (miss != missing_auth_data.end() && !miss->second.has_value()) {
        trace("Populate auth data error: {}",
              std::to_underlying(miss->second.error().code()));
        results[i] = miss->second.error().code();
      }
    }
  }
}

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

  AuthDataByAccount auth_data;
  bool had_misses = false;
  for (size_t i = 0; i < txs.size(); i++) {
    const SignedTransaction &tx = txs[i];
    if (wrapper_over_cap(tx)) {
      results[i] = AuthErrorCode::kAuthFailed;
      continue;
    }
    const ContractAddress &account = tx.tx.invocation.invoked_address;
    if (auth_data.contains(account)) {
      continue;
    }
    AuthCacheEntry entry = try_cached_auth_data(height, account);
    if (entry == nullptr) {
      // Keep all batching state outside the entirely cached request's
      // coroutine frame. The already collected snapshots stay request-owned.
      had_misses = true;
      co_await gather_missing_auth_data(height, txs, i, auth_data, results);
      break;
    }
    CHECK_PTR(entry.get());
    auth_data.emplace(account, std::move(entry));
  }

  // Use main's original chunk geometry and signature entry points.
  const std::vector<ChunkRange> chunks =
      make_chunks(txs.size(),
                  std::clamp<size_t>(txs.size() / kMinTxsPerVerifyChunk, 1,
                                     max_parallelism));
  const std::span<AuthErrorCode> result_span(results.data(), results.size());
  if (chunks.size() == 1) {
    // DB replies can resume on the reader. Do not hold it for signatures.
    if (had_misses) {
      co_await default_coropoll().pool_.bg();
    }
    verify_chunk(txs, result_span, auth_data, network_hash_);
    co_return results;
  }
  std::vector<x23::Async<void>> handles;
  handles.reserve(chunks.size());
  for (const ChunkRange &chunk : chunks) {
    const size_t count = chunk.end - chunk.start;
    handles.push_back(
        verify_chunk_on_pool(txs.subspan(chunk.start, count),
                             result_span.subspan(chunk.start, count), auth_data,
                             network_hash_));
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
    AuthCacheEntry source_addr =
        try_cached_auth_data(height, tx.tx.invocation.invoked_address);
    if (source_addr == nullptr) {
      auto looked_up =
          co_await lookup_auth_data(height, tx.tx.invocation.invoked_address);
      if (!looked_up.has_value()) {
        co_return looked_up.error().code();
      }
      source_addr = std::move(*looked_up);
    }
    CHECK_PTR(source_addr.get());
    const std::optional<AddressAuthData> &source_auth_data =
        source_addr->second;

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
