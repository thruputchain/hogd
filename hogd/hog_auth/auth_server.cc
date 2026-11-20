#include "hogd/hog_auth/auth_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "hogd/base/rpc_profile.h"
#include "hogd/base/chunk_items.h"
#include "hogd/base/chunk_position.h"
#include "hogd/base/chunk_run_sender.h"
#include "hogd/base/chunked_vector.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/key_change_chunk_view.h"
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
  rpc_profile::InFlight in_flight;
  const auto entered = std::chrono::steady_clock::now();
  co_await default_coropoll().pool_.bg();
  const auto started = std::chrono::steady_clock::now();

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

  const auto looked_up = std::chrono::steady_clock::now();
  // Experiment: self-scheduled verification. Up to max_parallelism claimants
  // each hop to the pool and claim kClaimTxs transactions at a time from a
  // shared cursor until none are left. A static split leaves a fixed share
  // behind any worker that starts late (a busy LLC queue keeps its jobs while
  // idle workers elsewhere sleep); here a late claimant finds nothing left.
  constexpr size_t kClaimTxs = 64;
  const size_t claimants = std::min(
      max_parallelism, std::max<size_t>(1, (txs.size() + kClaimTxs - 1) / kClaimTxs));
  std::atomic<size_t> cursor{0};
  std::vector<x23::Async<void>> handles;
  handles.reserve(claimants);
  rpc_profile::CpuSet cpus_used;
  for (size_t c = 0; c < claimants; ++c) {
    handles.push_back(
        [](std::atomic<size_t> &cursor, x23::span<const SignedTransaction> txs,
           const std::unordered_map<ContractAddress, AuthCacheEntry,
                                    SipHash24Hasher> &auth_data,
           NetworkHash network_hash, AuthErrorCodeBatch &results,
           std::chrono::steady_clock::time_point spawned,
           rpc_profile::CpuSet &cpus_used) noexcept -> x23::Async<void> {
          co_await default_coropoll().pool_.bg();
          rpc_profile::ChunkTimer chunk_timer(spawned, cpus_used);

          x23::Buf signature_preimage;
          uint64_t lookup_ns = 0;
          uint64_t verify_auth_ns = 0;
          uint64_t claims = 0;
          rpc_profile::tl_preimage_ns = 0;
          rpc_profile::tl_sig_ns = 0;
          for (;;) {
            const size_t start =
                cursor.fetch_add(kClaimTxs, std::memory_order_relaxed);
            if (start >= txs.size()) {
              break;
            }
            ++claims;
            const size_t end = std::min(start + kClaimTxs, txs.size());
            for (size_t i = start; i < end; i++) {
              const SignedTransaction &tx = txs[i];
              if (results[i] != AuthErrorCode::kAuthSuccess) {
                continue;
              }

              const auto lookup_started = std::chrono::steady_clock::now();
              const ContractAddress &account = tx.tx.invocation.invoked_address;
              const auto account_it = auth_data.find(account);
              CHECK(account_it != auth_data.end());
              const std::optional<AddressAuthData> &source_auth_data =
                  account_it->second->second;

              const auto verify_started = std::chrono::steady_clock::now();
              lookup_ns +=
                  rpc_profile::ns_between(lookup_started, verify_started);
              AuthVerifyStatus<void> verify_result = verify_auth(
                  tx, source_auth_data ? &*source_auth_data : nullptr,
                  network_hash, signature_preimage);
              verify_auth_ns += rpc_profile::ns_between(
                  verify_started, std::chrono::steady_clock::now());
              if (!verify_result.has_value()) {
                results[i] = AuthErrorCode::kAuthFailed;
                continue;
              }
              // [results] starts as all kAuthSuccess.
            }
          }
          {
            auto &prof = rpc_profile::profile();
            prof.phase_lookup.add(lookup_ns);
            prof.phase_verify_auth.add(verify_auth_ns);
            prof.phase_preimage.add(rpc_profile::tl_preimage_ns);
            prof.phase_sig.add(rpc_profile::tl_sig_ns);
            prof.claims.fetch_add(claims, std::memory_order_relaxed);
            if (claims == 0) {
              prof.empty_claimants.fetch_add(1, std::memory_order_relaxed);
            }
          }
          co_return;
        }(cursor, txs, auth_data, network_hash_, results, looked_up,
          cpus_used));
  }
  for (x23::Async<void> &handle : handles) {
    co_await handle;
  }
  const auto verified = std::chrono::steady_clock::now();
  auto &profile = rpc_profile::profile();
  profile.handler_queue.add(rpc_profile::ns_between(entered, started));
  profile.handler_lookup.add(rpc_profile::ns_between(started, looked_up));
  profile.handler_verify.add(rpc_profile::ns_between(looked_up, verified));
  profile.handler_total.add(rpc_profile::ns_between(entered, verified));
  profile.handler_txs.fetch_add(txs.size(), std::memory_order_relaxed);
  profile.chunks.fetch_add(claimants, std::memory_order_relaxed);
  profile.cpus_per_call_sum.fetch_add(cpus_used.count(),
                                      std::memory_order_relaxed);
  profile.llcs_per_call_sum.fetch_add(cpus_used.llc_count(),
                                      std::memory_order_relaxed);
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
