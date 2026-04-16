#pragma once

#include <map>
#include <utility>
#include <vector>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/perthread.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/refcnt.h"
#include "x23/sockio.h"

namespace hogd {

class AuthChangeSubscriptionManager {
public:
  x23::Detached
  detached_publish_auth_changes(BlockHeight height,
                                PerThread<KeyChangeBatch> key_changes);
  x23::Async<void> publish_auth_changes(BlockHeight height,
                                        PerThread<KeyChangeBatch> key_changes);
  x23::Async<bool> add_full_auth_change_subscriber(AuthCard card);

  ~AuthChangeSubscriptionManager() { inhibit_.destroy_wait(); }

private:
  using auth_db_client_t =
      decltype(make_rpc_client<AuthDB1>(x23::HostPort{})());

  static x23::Async<std::optional<auth_db_client_t>>
  create_client(const sockaddr_storage &address, const AuthCard &card);

  // Under lock_: kick off authdb_forward_key_changes on every current client,
  // returning (card, future) pairs. A client whose dispatch throws
  // synchronously is erased from the map before returning. Lock is released
  // when this coroutine returns — the caller awaits the futures unlocked.
  x23::Async<std::vector<std::pair<AuthCard, x23::Async<void>>>>
  start_forward_key_changes(BlockHeight height, const KeyChangeBatch &batch);

  // Under lock_: erase one client. Used on post-await RPC failure.
  x23::Async<void> erase_client(const AuthCard &card);

  // Under lock_: replace-or-insert one client.
  x23::Async<void> insert_client(AuthCard card, auth_db_client_t client);

  x23::CoroLock lock_;
  std::map<AuthCard, auth_db_client_t> auth_db_clients_;
  x23::DestroyInhibit inhibit_;
};

} // namespace hogd
