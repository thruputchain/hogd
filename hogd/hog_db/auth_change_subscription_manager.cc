#include "auth_change_subscription_manager.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_message_size.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/format.h"
#include "x23/refcnt.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

x23::Detached
AuthChangeSubscriptionManager::detached_publish_auth_changes(
    BlockHeight height, PerThread<KeyChangeBatch> key_changes)
{
  // NOLINTNEXTLINE(readability-identifier-naming)
  x23::RefPtr _inhibit(inhibit_);
  co_await default_threadpool().bg();
  co_await publish_auth_changes(height, std::move(key_changes));
}

x23::Async<void>
AuthChangeSubscriptionManager::publish_auth_changes(
    BlockHeight height, PerThread<KeyChangeBatch> key_changes)
{
  KeyChangeBatch combined_batch;
  {
    auto ul = key_changes.lock_threads();
    key_changes.for_each(ul, [&](const KeyChangeBatch &batch) {
      combined_batch.append_range(batch);
    });
  }

  auto pending = co_await start_forward_key_changes(height, combined_batch);

  for (auto &[card, client_ref, future] : pending) {
    bool failed = false;
    try {
      co_await future;
    } catch (const x23::XdrException &e) {
      error("AuthChangeSubscriptionManager: Bad response from auth server {}: "
            "{}.  Closing connection.",
            x23::show(card), e.what());
      failed = true;
    }
    if (failed) {
      // Per-iteration erase matches the pre-fix cleanup timing: a known-bad
      // client is removed before later iterations could dispatch to it
      // (though re-dispatch within a single publish_auth_changes call is not
      // actually possible, a concurrent caller could observe the bad client).
      // client_ref holds the RpcStream alive; its address is a valid identity
      // for the CAS check inside erase_client_if_stale.
      co_await erase_client_if_stale(card, client_ref.get());
    }
  }
}

x23::Async<std::optional<AuthChangeSubscriptionManager::auth_db_client_t>>
AuthChangeSubscriptionManager::create_client(const sockaddr_storage &address,
                                             const AuthCard &card)
{
  // Match the auth service's inbound cap (auth.cc sets its AuthDBRpc server's
  // max_message_size_ to a large value). A committed block can produce a
  // KeyChangeBatch larger than the default 16 MiB cap; without this the
  // outbound send throws locally and the subscriber is erased, stalling auth
  // delivery for later heights (issue #3146).
  auto client =
      co_await make_rpc_client<AuthDB1>(address, false, kBulkRpcMaxMessageSize);
  if (!client) {
    error("AuthChangeSubscriptionManager: Failed to create RPC client for "
          "{}: {}",
          card.address_for_db, client.error().error_code().message());
    co_return std::nullopt;
  }
  co_return std::move(*client);
}

x23::Async<std::optional<BlockHeight>>
AuthChangeSubscriptionManager::add_full_auth_change_subscriber(AuthCard card)
{

  x23::HostPort helper;
  if (!helper.parse(card.address_for_db)) {
    error("AuthChangeSubscriptionManager: Failed to parse address {}",
          card.address_for_db);
    co_return std::nullopt;
  }

  auto addr = helper.to_addr(/*dns=*/true);
  if (!addr) {
    error("AuthChangeSubscriptionManager: Failed to convert address {} to "
          "sockaddr_storage",
          card.address_for_db);
    co_return std::nullopt;
  }

  std::optional<auth_db_client_t> client = co_await create_client(*addr, card);
  if (!client) {
    error("AuthChangeSubscriptionManager: Failed to create client for {} on "
          "address {}",
          card.address_for_db, x23::show_ipaddr(*addr));
    co_return std::nullopt;
  }

  co_return co_await insert_client(std::move(card), std::move(*client));
}

x23::Async<std::vector<AuthChangeSubscriptionManager::PendingPublish>>
AuthChangeSubscriptionManager::start_forward_key_changes(
    BlockHeight height, const KeyChangeBatch &batch)
{
  std::vector<PendingPublish> pending;

  x23::CoroGuard guard;
  co_await guard(lock_);

  // Record the watermark under lock_, in the same critical section that
  // captures the subscriber set. A subscriber inserted after this point has
  // missed height [height]; insert_client reports this watermark so the intro
  // height can cover it rather than parking on it forever (issue #2814).
  // Detached publishes can run out of order, so take the max, not the last.
  last_published_height_ = std::max(last_published_height_, height);

  pending.reserve(auth_db_clients_.size());
  for (auto it = auth_db_clients_.begin(); it != auth_db_clients_.end();) {
    auto &client = it->second;
    try {
      pending.push_back(
          PendingPublish{.card = it->first,
                         .client_ref = client.__send_call,
                         .future =
                             client.authdb_forward_key_changes(height, batch)});
      ++it;
    } catch (x23::XdrException &e) {
      error("AuthChangeSubscriptionManager: Failed to forward key changes to "
            "auth server {}: {}.  Closing connection.",
            x23::show(it->first), e.what());
      it = auth_db_clients_.erase(it);
    }
  }
  co_return pending;
}

x23::Async<void>
AuthChangeSubscriptionManager::erase_client_if_stale(
    const AuthCard &card, const rpc_stream_t *expected_client_id)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  auto it = auth_db_clients_.find(card);
  if (it != auth_db_clients_.end() &&
      it->second.__send_call.get() == expected_client_id) {
    auth_db_clients_.erase(it);
  }
}

x23::Async<BlockHeight>
AuthChangeSubscriptionManager::insert_client(AuthCard card,
                                             auth_db_client_t client)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  auth_db_clients_.erase(card);
  [[maybe_unused]] auto [_, inserted] =
      auth_db_clients_.emplace(std::move(card), std::move(client));
  assert(inserted);
  co_return last_published_height_;
}

} // namespace hogd
