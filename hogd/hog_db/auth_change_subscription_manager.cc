#include "auth_change_subscription_manager.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <sys/socket.h>
#include <type_traits>
#include <utility>
#include <vector>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/chunk_items.h"
#include "hogd/base/chunk_position.h"
#include "hogd/base/chunk_run_sender.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/format.h"
#include "x23/refcnt.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {
namespace {
// A KeyChangeChunk whose changes are still in the per-thread batches they
// were produced in. It serializes identically to the wire struct, so the
// client sends it as one without copying a change.
struct KeyChangeChunkView {
  BlockID block_id;
  BlockHeight height;
  std::uint32_t chunk_index;
  std::uint32_t chunk_count;
  ChunkItems<KeyChange> changes;
};

size_t
xdr_size(const KeyChangeChunkView &view)
{
  return x23::xdr_size(view.block_id) + x23::xdr_size(view.height) +
         x23::xdr_size(view.chunk_index) + x23::xdr_size(view.chunk_count) +
         xdr_size(view.changes);
}

void
xdr_serialize(auto &serializer, const KeyChangeChunkView &view)
{
  serializer(view.block_id);
  serializer(view.height);
  serializer(view.chunk_index);
  serializer(view.chunk_count);
  serializer(view.changes);
}
} // namespace
} // namespace hogd

template<>
struct x23::serialize_equiv<hogd::KeyChangeChunk, hogd::KeyChangeChunkView>
  : std::true_type {};

namespace hogd {

x23::Async<void>
AuthChangeSubscriptionManager::ForwardKeyChangeChunk::operator()(
    ChunkPosition position, ChunkItems<KeyChange> changes)
{
  // The record is serialized from [changes] in place, before the call first
  // suspends.
  const KeyChangeChunkView view{.block_id = tip.block_id,
                                .height = tip.height,
                                .chunk_index = position.index,
                                .chunk_count = position.count,
                                .changes = changes};
  co_await client.authdb_forward_key_changes_chunk(view);
}

x23::Detached
AuthChangeSubscriptionManager::detached_publish_auth_changes(
    DBAuthTip tip, PerThread<KeyChangeBatch> key_changes)
{
  // NOLINTNEXTLINE(readability-identifier-naming)
  x23::RefPtr _inhibit(inhibit_);
  co_await default_threadpool().bg();
  co_await publish_auth_changes(tip, std::move(key_changes));
}

x23::Async<void>
AuthChangeSubscriptionManager::publish_auth_changes(
    DBAuthTip tip, PerThread<KeyChangeBatch> key_changes)
{
  // Each thread's batch is a part of the run as it is; the chunks are
  // serialized straight from these, so they live until every run is done.
  std::vector<KeyChangeBatch> parts;
  {
    auto ul = key_changes.lock_threads();
    key_changes.for_each(ul, [&parts](KeyChangeBatch &batch) {
      parts.push_back(std::move(batch));
    });
  }

  auto pending = co_await start_forward_key_changes(tip, parts);

  for (auto &[card, client_ref, sender, future] : pending) {
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
  // Match the auth service's inbound cap (hog_auth.cc sets its AuthDBRpc
  // server's max_message_size_ to a large value). Key changes go out as
  // KeyChangeChunk records sized under the default 16 MiB cap, so this is
  // headroom rather than a requirement; the whole-batch forward it was added
  // for (issue #3146) is gone.
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

x23::Async<std::optional<DBAuthTip>>
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
    DBAuthTip tip, const std::vector<KeyChangeBatch> &parts)
{
  std::vector<PendingPublish> pending;

  x23::CoroGuard guard;
  co_await guard(lock_);

  // Record the watermark under lock_, in the same critical section that
  // captures the subscriber set. A subscriber inserted after this point has
  // missed height [height]; insert_client reports this watermark so the intro
  // height can cover it rather than parking on it forever (issue #2814).
  // Detached publishes can run out of order, so take the max, not the last.
  if (tip.height > last_published_tip_.height) {
    last_published_tip_ = tip;
  }

  // A run's chunks are serialized and sent on the pool, so a failure, such as
  // a record over the cap, reaches the caller through the future rather than
  // here.
  pending.reserve(auth_db_clients_.size());
  for (auto &[card, client] : auth_db_clients_) {
    auto sender = std::make_unique<KeyChangeRunSender>(
        parts, max_key_changes_per_chunk_,
        ForwardKeyChangeChunk{.client = client, .tip = tip});
    x23::Async<void> future = sender->send();
    pending.push_back(PendingPublish{.card = card,
                                     .client_ref = client.__send_call,
                                     .sender = std::move(sender),
                                     .future = std::move(future)});
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

x23::Async<DBAuthTip>
AuthChangeSubscriptionManager::insert_client(AuthCard card,
                                             auth_db_client_t client)
{
  x23::CoroGuard guard;
  co_await guard(lock_);
  auth_db_clients_.erase(card);
  [[maybe_unused]] auto [_, inserted] =
      auth_db_clients_.emplace(std::move(card), std::move(client));
  assert(inserted);
  co_return last_published_tip_;
}

} // namespace hogd
