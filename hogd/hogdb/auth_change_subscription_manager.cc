#include "auth_change_subscription_manager.h"

#include <cassert>
#include <exception>
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

  auto futures = co_await start_forward_key_changes(height, combined_batch);

  std::vector<AuthCard> to_erase;
  for (auto &[card, future] : futures) {
    try {
      co_await future;
    } catch (const x23::XdrException &e) {
      error("AuthChangeSubscriptionManager: Bad response from auth server {}: "
            "{}.  Closing connection.",
            x23::show(card), e.what());
      to_erase.push_back(card);
    }
  }

  for (auto &card : to_erase) {
    co_await erase_client(card);
  }
}

x23::Async<std::optional<AuthChangeSubscriptionManager::auth_db_client_t>>
AuthChangeSubscriptionManager::create_client(const sockaddr_storage &address,
                                             const AuthCard &card)
{
  try {
    co_return co_await make_rpc_client<AuthDB1>(address, false);
  } catch (const std::exception &e) {
    error("AuthChangeSubscriptionManager: Failed to create RPC client for "
          "{}: {}",
          card.address_for_db, e.what());
    co_return std::nullopt;
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

  std::optional<auth_db_client_t> client = co_await create_client(*addr, card);
  if (!client) {
    error("AuthChangeSubscriptionManager: Failed to create client for {} on "
          "address {}",
          card.address_for_db, x23::show_ipaddr(*addr));
    co_return false;
  }

  co_await insert_client(std::move(card), std::move(*client));
  co_return true;
}

x23::Async<std::vector<std::pair<AuthCard, x23::Async<void>>>>
AuthChangeSubscriptionManager::start_forward_key_changes(
    BlockHeight height, const KeyChangeBatch &batch)
{
  std::vector<std::pair<AuthCard, x23::Async<void>>> futures;

  x23::CoroGuard guard;
  co_await guard(lock_);

  futures.reserve(auth_db_clients_.size());
  for (auto it = auth_db_clients_.begin(); it != auth_db_clients_.end();) {
    auto &client = it->second;
    try {
      futures.emplace_back(it->first,
                           client.authdb_forward_key_changes(height, batch));
      ++it;
    } catch (x23::XdrException &e) {
      error("AuthChangeSubscriptionManager: Failed to forward key changes to "
            "auth server {}: {}.  Closing connection.",
            x23::show(it->first), e.what());
      it = auth_db_clients_.erase(it);
    }
  }
  co_return futures;
}

x23::Async<void>
AuthChangeSubscriptionManager::erase_client(const AuthCard &card)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  auth_db_clients_.erase(card);
}

x23::Async<void>
AuthChangeSubscriptionManager::insert_client(AuthCard card,
                                             auth_db_client_t client)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  auth_db_clients_.erase(card);
  [[maybe_unused]] auto [_, inserted] =
      auth_db_clients_.emplace(std::move(card), std::move(client));
  assert(inserted);
  co_return;
}

} // namespace hogd
