#include "hogd/auth/auth_server.h"

#include <cassert>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "account.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/auth/status.h"
#include "hogd/auth_claim/verify.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<AuthStatus<
    std::shared_ptr<std::pair<BlockHeight, std::optional<AccountAuthData>>>>>
AuthServer::lookup_auth_data(BlockHeight height, ContractAddress address)
{
  assert(height != kUninitializedHeight);
  assert(height != kInitializing);
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
    break;
  }

  CHECK(current_height != kUninitializedHeight);
  CHECK(current_height != kInitializing);

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
      std::make_shared<std::pair<BlockHeight, std::optional<AccountAuthData>>>(
          height, db_auth_data.value());

  auth_data_cache_.put(address, entry);

  co_return entry;
}

x23::Async<void>
AuthServer::register_auth_data_updates(BlockHeight height,
                                       KeyChangeBatch const &key_changes)
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
          std::pair<BlockHeight, std::optional<AccountAuthData>>>(height,
                                                                  std::nullopt);
      auth_data_cache_.put(key_change.address, entry);
      continue;
    }
    auto entry = std::make_shared<
        std::pair<BlockHeight, std::optional<AccountAuthData>>>(
        height, *key_change.auth_data);
    auth_data_cache_.put(key_change.address, entry);
  }
  current_height_futex_.store(height);
  current_height_futex_.broadcast(&default_threadpool());
}

x23::Async<void>
AuthServer::subscribe_to_auth_data_changes(AuthCard const &card)
{
  return dbauth_client_.subscribe_to_auth_data_changes(card);
}

x23::Async<AuthErrorCode>
AuthServer::populate_auth_data(BlockHeight height,
                               ContractAddress const &address,
                               const AccountAuthData *&auth_data)
{
  auto entry = co_await lookup_auth_data(height, address);
  if (!entry.has_value()) {
    co_return entry.error().code();
  }

  CHECK(entry.value().get() != nullptr);

  if (!entry.value()->second.has_value()) {
    co_return AuthErrorCode::kAuthFailed;
  }

  auth_data = &(entry.value()->second.value());
  co_return AuthErrorCode::kAuthSuccess;
}

x23::Async<AuthErrorCodeBatch>
AuthServer::verify_transactions(BlockHeight height, SignedTransactionBatch txs)
{
  AuthErrorCodeBatch result;
  result.reserve(txs.size());
  for (const auto &tx : txs) {
    const AccountAuthData *source_auth_data;
    {
      auto error =
          co_await populate_auth_data(height, tx.tx.invocation.invoked_address,
                                      source_auth_data);
      if (error != AuthErrorCode::kAuthSuccess) {
        result.push_back(error);
        continue;
      }
    }

    CHECK(source_auth_data != nullptr);

    auto relevant_contracts = get_aux_addresses(tx.tx);
    std::unordered_map<ContractAddress, const AccountAuthData *> aux_auth_data;
    for (const auto &contract : relevant_contracts) {
      auto error = co_await populate_auth_data(height, contract,
                                               aux_auth_data[contract]);
      if (error != AuthErrorCode::kAuthSuccess) {
        result.push_back(error);
        continue;
      }
    }

    auto verify_result = verify_auth(tx, *source_auth_data, aux_auth_data);

    if (!verify_result.has_value()) {
      result.push_back(AuthErrorCode::kAuthFailed);
      continue;
    }

    result.push_back(AuthErrorCode::kAuthSuccess);
  }
  co_return result;
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
    CHECK(source_addr.value().get() != nullptr);
    if (!source_addr.value()->second.has_value()) {
      co_return AuthErrorCode::kAuthFailed;
    }
    const AccountAuthData *source_auth_data =
        &(source_addr.value()->second.value());
    auto relevant_contracts = get_aux_addresses(tx.tx);
    std::unordered_map<ContractAddress, const AccountAuthData *> aux_auth_data;
    for (const auto &contract : relevant_contracts) {
      auto error = co_await populate_auth_data(height, contract,
                                               aux_auth_data[contract]);
      if (error != AuthErrorCode::kAuthSuccess) {
        co_return error;
      }
    }

    auto verify_result = verify_auth(tx, *source_auth_data, aux_auth_data);
    if (!verify_result.has_value()) {
      co_return AuthErrorCode::kAuthFailed;
    }
  }
  if (signature_verifier.verify()) {
    co_return AuthErrorCode::kAuthSuccess;
  }
  co_return AuthErrorCode::kAuthFailed;
}
} // namespace hogd
