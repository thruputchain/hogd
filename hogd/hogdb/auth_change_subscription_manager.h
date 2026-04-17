#pragma once

#include <map>
#include <tuple>
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
#include "x23/rpc.h"
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
  using rpc_stream_t = x23::RpcStream<x23::RpcLogger>;
  using client_ref_t = rpc_stream_t::ClientRef;

  // One in-flight forward-RPC: (card, client_ref, future). The ClientRef is
  // copied from the map entry under lock_ to keep the RpcStream alive for the
  // lifetime of this tuple — the returned Async<void> holds only a raw
  // RpcStream* in its coroutine frame, so we cannot rely on the future alone
  // to preserve identity. client_ref.get() is the stream address used for
  // post-await identity-equality comparison in erase_client_if_stale.
  using pending_publish_t =
      std::tuple<AuthCard, client_ref_t, x23::Async<void>>;

  static x23::Async<std::optional<auth_db_client_t>>
  create_client(const sockaddr_storage &address, const AuthCard &card);

  // Under lock_: kick off authdb_forward_key_changes on every current client,
  // returning a pending_publish_t per client. A client whose dispatch throws
  // synchronously is erased from the map before returning. Lock is released
  // when this coroutine returns — the caller awaits the futures unlocked.
  x23::Async<std::vector<pending_publish_t>>
  start_forward_key_changes(BlockHeight height, const KeyChangeBatch &batch);

  // Under lock_: erase the entry for `card` only if it still holds the client
  // whose underlying RpcStream was `expected_client_id`. The identity check
  // prevents a post-RPC-failure erase from silently deleting a healthy client
  // that a concurrent add_full_auth_change_subscriber inserted in the meantime.
  x23::Async<void>
  erase_client_if_stale(const AuthCard &card,
                        const rpc_stream_t *expected_client_id);

  // Under lock_: replace-or-insert one client.
  x23::Async<void> insert_client(AuthCard card, auth_db_client_t client);

  x23::CoroLock lock_;
  std::map<AuthCard, auth_db_client_t> auth_db_clients_;
  x23::DestroyInhibit inhibit_;
};

} // namespace hogd
