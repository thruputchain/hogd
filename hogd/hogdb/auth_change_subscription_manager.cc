#include "auth_change_subscription_manager.h"

#include <cassert>
#include <exception>
#include <optional>
#include <utility>
#include <vector>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/rpc_client.h"
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

  x23::CoroGuard guard;
  co_await guard(lock_);

  std::vector<std::pair<AuthCard, x23::Async<void>>> futures;
  futures.reserve(auth_db_clients_.size());
  for (auto it = auth_db_clients_.begin(); it != auth_db_clients_.end();) {
    auto &client = it->second;
    try {
      futures.emplace_back(it->first,
                           client.authdb_forward_key_changes(height,
                                                             combined_batch));
    } catch (x23::XdrException &e) {
      error("AuthChangeSubscriptionManager: Failed to forward key changes to "
            "auth server {}: {}.  Closing connection.",
            x23::show(it->first), e.what());
      it = auth_db_clients_.erase(it);
      continue;
    }
    ++it;
  }

  for (auto &[card, future] : futures) {
    try {
      co_await future;
    } catch (const x23::XdrException &e) {
      error("AuthChangeSubscriptionManager: Bad response from auth server {}: "
            "{}.  Closing connection.",
            x23::show(card), e.what());
      auth_db_clients_.erase(card);
      continue;
    }
  }
}

x23::Async<bool>
AuthChangeSubscriptionManager::add_full_auth_change_subscriber(AuthCard card)
{

  x23::HostPort helper;
  if (!helper.parse(card.address_for_db)) {
    error("AuthChangeSubscriptionManager: Failed to parse address {}",
          card.address_for_db);
    co_return false;
  }

  auto addr = helper.to_addr(/*dns=*/true);
  if (!addr) {
    error("AuthChangeSubscriptionManager: Failed to convert address {} to "
          "sockaddr_storage",
          card.address_for_db);
    co_return false;
  }

  std::optional<auth_db_client_t> client =
      co_await [&]() -> x23::Async<std::optional<auth_db_client_t>> {
    try {
      info("AuthChangeSubscriptionManager: Creating RPC client for {}", card.address_for_db);
      co_return co_await make_rpc_client<AuthDB1>(*addr, false);
    } catch (const std::exception &e) {
      error("AuthChangeSubscriptionManager: Failed to create RPC client for "
            "{}: {}",
            card.address_for_db, e.what());
      co_return std::nullopt;
    }
  }();

  if (!client) {
    co_return false;
  }

  x23::CoroGuard guard;
  co_await guard(lock_);

  auth_db_clients_.erase(card);
  [[maybe_unused]] auto [_, inserted] =
      auth_db_clients_.emplace(card, std::move(*client));
  assert(inserted);
  co_return true;
}

} // namespace hogd
