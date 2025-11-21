#include "auth_change_subscription_manager.h"

#include <cassert>
#include <utility>
#include <vector>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/format.h"
#include "x23/refcnt.h"
#include "x23/vec.h"

namespace hogd {

void
AuthChangeSubscriptionManager::detached_publish_auth_changes(
    BlockHeight height, PerThread<KeyChangeBatch> key_changes)
{
  [this](this auto, BlockHeight height,
         PerThread<KeyChangeBatch> key_changes) -> x23::Detached {
    x23::RefPtr guard = this->inhibit_;
    co_await default_threadpool().bg();
    co_await publish_auth_changes(height, std::move(key_changes));
  }(height, std::move(key_changes));
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

x23::Async<void>
AuthChangeSubscriptionManager::add_full_auth_change_subscriber(AuthCard card)
{
  x23::CoroGuard guard;
  co_await guard(lock_);

  // Create RPC client from the AuthCard's address
  auto sockaddr = server_address_to_sockaddr(card.address);
  auto client = make_rpc_client<AuthDB1>(sockaddr, false);
  auth_db_clients_.erase(card);
  [[maybe_unused]] auto [_, inserted] =
      auth_db_clients_.emplace(card, std::move(client));
  assert(inserted);
  co_return;
}

} // namespace hogd
