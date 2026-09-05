#pragma once

#include <map>
#include <optional>
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
  // Adds a subscriber. Returns nullopt on failure. On success, returns the
  // highest auth-change height already dispatched to other subscribers at the
  // moment this one was inserted (-1 if none) — the caller must report an intro
  // height >= this value so the new subscriber doesn't park forever on a height
  // whose publish it missed (issue #2814).
  x23::Async<std::optional<BlockHeight>>
  add_full_auth_change_subscriber(AuthCard card);

  ~AuthChangeSubscriptionManager() { inhibit_.destroy_wait(); }

private:
  using auth_db_client_t = rpc_client_t<AuthDB1>;
  using rpc_stream_t = x23::RpcStream<x23::RpcLogger>;
  using client_ref_t = rpc_stream_t::ClientRef;

  // One in-flight forward-RPC. Member declaration order matters: ~future
  // runs before ~client_ref, keeping the RpcStream alive while the coroutine
  // handle (which holds only a raw RpcStream*) is destroyed.
  struct PendingPublish { // NOLINT(bugprone-exception-escape)
    AuthCard card;
    client_ref_t client_ref;
    x23::Async<void> future;
  };

  static x23::Async<std::optional<auth_db_client_t>>
  create_client(const sockaddr_storage &address, const AuthCard &card);

  // Under lock_: kick off authdb_forward_key_changes on every current client,
  // returning a PendingPublish per client. A client whose dispatch throws
  // synchronously is erased from the map before returning. Lock is released
  // when this coroutine returns — the caller awaits the futures unlocked.
  x23::Async<std::vector<PendingPublish>>
  start_forward_key_changes(BlockHeight height, const KeyChangeBatch &batch);

  // Under lock_: erase the entry for `card` only if it still holds the client
  // whose underlying RpcStream was `expected_client_id`. The identity check
  // prevents a post-RPC-failure erase from silently deleting a healthy client
  // that a concurrent add_full_auth_change_subscriber inserted in the meantime.
  x23::Async<void>
  erase_client_if_stale(const AuthCard &card,
                        const rpc_stream_t *expected_client_id);

  // Under lock_: replace-or-insert one client. Returns last_published_height_
  // captured under the same lock, so the insert and the reported watermark are
  // consistent w.r.t. publishes (issue #2814).
  x23::Async<BlockHeight> insert_client(AuthCard card, auth_db_client_t client);

  x23::CoroLock lock_;
  std::map<AuthCard, auth_db_client_t> auth_db_clients_;
  // Highest height passed to start_forward_key_changes so far (-1 if none).
  // Guarded by lock_. Lets a newly added subscriber learn the highest publish
  // it may have just missed, so its intro height can cover it (issue #2814).
  BlockHeight last_published_height_ = -1;
  x23::DestroyInhibit inhibit_;
};

} // namespace hogd
