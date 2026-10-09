#pragma once

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/chunk_items.h"
#include "hogd/base/chunk_position.h"
#include "hogd/base/chunk_run_sender.h"
#include "hogd/base/perthread.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
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
  // The most key changes one KeyChangeChunk carries. A KeyChange is 1.1 KB on
  // the wire at most (21 signer slots), so a chunk stays under 9 MiB, below
  // x23's 16 MiB default record cap.
  static constexpr size_t kMaxKeyChangesPerChunk = size_t{1} << 13;

  // [max_key_changes_per_chunk] is a test seam; production uses the default.
  explicit AuthChangeSubscriptionManager(
      size_t max_key_changes_per_chunk = kMaxKeyChangesPerChunk)
    : max_key_changes_per_chunk_(max_key_changes_per_chunk)
  {}

  // Sends a committed block's auth-data changes to every subscriber as a run
  // of KeyChangeChunk records, each thread's batch a part of the run, and
  // returns once every subscriber has acknowledged every chunk.
  x23::Detached
  detached_publish_auth_changes(DBAuthTip tip,
                                PerThread<KeyChangeBatch> key_changes);
  x23::Async<void> publish_auth_changes(DBAuthTip tip,
                                        PerThread<KeyChangeBatch> key_changes);
  // Adds a subscriber. Returns nullopt on failure. On success, returns the
  // highest auth-change height already dispatched to other subscribers at the
  // moment this one was inserted (-1 if none) — the caller must report an intro
  // height >= this value so the new subscriber doesn't park forever on a height
  // whose publish it missed (issue #2814).
  x23::Async<std::optional<DBAuthTip>>
  add_full_auth_change_subscriber(AuthCard card);

  ~AuthChangeSubscriptionManager() { inhibit_.destroy_wait(); }

private:
  using auth_db_client_t = rpc_client_t<AuthDB1>;
  using rpc_stream_t = x23::RpcStream<x23::RpcLogger>;
  using client_ref_t = rpc_stream_t::ClientRef;

  // Issues one chunk of [height]'s run to one subscriber. It holds its own
  // copy of the client, so a run outlives the map entry it was started from.
  struct ForwardKeyChangeChunk {
    auth_db_client_t client;
    DBAuthTip tip;

    x23::Async<void> operator()(ChunkPosition position,
                                ChunkItems<KeyChange> changes);
  };
  using KeyChangeRunSender = ChunkRunSender<KeyChange, ForwardKeyChangeChunk>;

  // One subscriber's in-flight run. Member declaration order matters: ~future
  // runs before ~sender and ~client_ref, keeping the RpcStream alive while the
  // coroutine handle (which holds only a raw RpcStream*) is destroyed.
  struct PendingPublish { // NOLINT(bugprone-exception-escape)
    AuthCard card;
    client_ref_t client_ref;
    std::unique_ptr<KeyChangeRunSender> sender;
    x23::Async<void> future;
  };

  static x23::Async<std::optional<auth_db_client_t>>
  create_client(const sockaddr_storage &address, const AuthCard &card);

  // Under lock_: start a run over [parts] to every current client, returning
  // a PendingPublish per client. Lock is released when this coroutine returns;
  // the caller awaits the futures unlocked, and [parts] must outlive them.
  x23::Async<std::vector<PendingPublish>>
  start_forward_key_changes(DBAuthTip tip,
                            const std::vector<KeyChangeBatch> &parts);

  // Under lock_: erase the entry for `card` only if it still holds the client
  // whose underlying RpcStream was `expected_client_id`. The identity check
  // prevents a post-RPC-failure erase from silently deleting a healthy client
  // that a concurrent add_full_auth_change_subscriber inserted in the meantime.
  x23::Async<void>
  erase_client_if_stale(const AuthCard &card,
                        const rpc_stream_t *expected_client_id);

  // Under lock_: replace-or-insert one client. Returns last_published_tip_
  // captured under the same lock, so the insert and the reported watermark are
  // consistent w.r.t. publishes (issue #2814).
  x23::Async<DBAuthTip> insert_client(AuthCard card, auth_db_client_t client);

  size_t max_key_changes_per_chunk_;
  x23::CoroLock lock_;
  std::map<AuthCard, auth_db_client_t> auth_db_clients_;
  // Highest height passed to start_forward_key_changes so far (-1 if none).
  // Guarded by lock_. Lets a newly added subscriber learn the highest publish
  // it may have just missed, so its intro height can cover it (issue #2814).
  DBAuthTip last_published_tip_{.block_id = 0, .height = -1};
  x23::DestroyInhibit inhibit_;
};

} // namespace hogd
