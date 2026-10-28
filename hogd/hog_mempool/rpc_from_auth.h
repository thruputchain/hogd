#pragma once

#include <cstddef>
#include <optional>
#include <utility>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/chunk_position.h"
#include "hogd/base/chunk_run_assembler.h"
#include "hogd/base/chunked_vector.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/version.h"
#include "hogd/xdr/protocol_token.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/x23/corosync.h"

namespace hogd {

class MempoolAuthRpc {
public:
  using rpc_service = MempoolAuth1;
  using auth_data_callback =
      std::function<x23::Async<void>(BlockHeight,
                                     ChunkedVector<KeyChangeBatch>)>;

  MempoolAuthRpc() = default;

  // A fresh scheduler has no cached transactions, so auth updates received
  // before coordinator initialization only contribute a height watermark.
  template<typename OnRegistered>
  x23::Async<void> register_callback(auth_data_callback &&callback,
                                     OnRegistered &&on_registered)
  {
    x23::CoroGuard cg;
    co_await cg(callback_lock_);

    CHECK(!callback_.has_value());
    callback_ = std::move(callback);
    callback_set_.store(true);
    callback_set_.broadcast(&default_threadpool());
    std::forward<OnRegistered>(on_registered)();
  }

  // Stop admitting callbacks that can touch coordinator state, then wait for
  // callbacks admitted before the close to finish. The coordinator calls this
  // before destroying members captured by [callback_].
  x23::Async<void> unregister_callback()
  {
    x23::CoroGuard cg;
    co_await cg(callback_lock_);

    callback_.reset();
    callback_set_.store(false);
    while (true) {
      size_t callbacks_in_flight = callbacks_in_flight_.load();
      if (callbacks_in_flight) {
        cg.reset();
        co_await callbacks_in_flight_.wait(callbacks_in_flight);
        co_await cg(callback_lock_);
        continue;
      }
      break;
    }
  }

  // Registering is what lets the mempool start serving the coordinator, so an
  // auth server calls this only once it has subscribed to the db. Only one
  // auth server is supported, so a second registration fails.
  static x23::Async<IntroAuthResult> mempoolauth_intro_auth(AuthCard card)
  {
    IntroAuthResult result = auth_info(card.protocol);
    if (result.outcome() == IntroAuthOutcome::kSuccess &&
        !Registration::registration().register_auth(card)) {
      warn("Rejecting auth registration: an auth server is already "
           "registered");
      co_return IntroAuthResult(IntroAuthOutcome::kFailure);
    }
    co_return result;
  }

  // What an auth server needs to subscribe to the db before it registers.
  static x23::Async<IntroAuthResult>
  mempoolauth_get_auth_info(ProtocolCommit protocol)
  {
    co_return auth_info(protocol);
  }

  // Collects the run of chunks the auth server sends for a height and runs
  // the callback once per complete height. Chunks may arrive in any order.
  x23::Async<void> mempoolauth_auth_data_changed_chunk(KeyChangeChunk chunk)
  {
    std::optional<KeyChangeRunAssembler::Run> run =
        run_assembler_.push(chunk.height,
                            ChunkPosition{.index = chunk.chunk_index,
                                          .count = chunk.chunk_count},
                            std::move(chunk.changes));
    if (!run.has_value()) {
      co_return;
    }
    const BlockHeight height = run->key;
    ChunkedVector<KeyChangeBatch> key_changes = std::move(run->parts);

    std::optional<auth_data_callback> callback;
    while (true) {
      x23::CoroGuard cg;
      co_await cg(callback_lock_);

      if (!callback_) {
        info("Recording auth-data watermark {} before mempool coordinator "
             "initialization; discarding {} key changes for an empty "
             "scheduler",
             height, key_changes.size());
        cg.reset();
        co_await callback_set_.wait(false);
        continue;
      }
      callback = callback_;
      callbacks_in_flight_.rmw([](size_t &count) { ++count; });
      break;
    }

    co_await (*callback)(height, std::move(key_changes));

    {
      x23::CoroGuard cg;
      co_await cg(callback_lock_);

      callbacks_in_flight_.rmw([](size_t &count) {
        CHECK_GT(count, size_t{0});
        --count;
      });
      callbacks_in_flight_.broadcast(&default_threadpool());
    }
  }

  static x23::Async<auth_sched_client_t> get_client()
  {
    AuthCard card;
    CHECK(Registration::registration().auth_registered(&card));

    auto ret = co_await create_client<AuthMempool1>(
        server_address_to_sockaddr(card.address_for_mempool), /*retry=*/false,
        /*max_message_size=*/kBulkRpcMaxMessageSize);
    if (!ret)
      panic("rpc_from_auth get_client connect failed: {}",
            ret.error().error_code().message());
    co_return std::move(*ret);
  }

private:
  // The registered db shards and the network hash, or a failure if
  // [protocol] is not ours.
  static IntroAuthResult auth_info(const ProtocolCommit &protocol)
  {
    info("got {}, expected {}", protocol_text(protocol),
         protocol_text(version::kHogdProtocol()));
    if (protocol != version::kHogdProtocol()) {
      return IntroAuthResult(IntroAuthOutcome::kFailure);
    }

    x23::XdrVec<ShardCard> shard_cards;
    Registration::registration().shards_registered(&shard_cards);
    NetworkHash network_hash{};
    Registration::registration().get_network_hash(&network_hash);
    IntroAuthResult result(IntroAuthOutcome::kSuccess);
    result.success() = MempoolAuthIntroSuccess{
        .shards = shard_cards,
        .network_hash = network_hash,
    };
    return result;
  }

  using KeyChangeRunAssembler = ChunkRunAssembler<BlockHeight, KeyChangeBatch>;

  KeyChangeRunAssembler run_assembler_;

  x23::CoroLock callback_lock_;
  std::optional<auth_data_callback> callback_;
  x23::CoroFutex<bool> callback_set_{false};
  x23::CoroFutex<size_t> callbacks_in_flight_{0};
};

static_assert(is_MempoolAuth1<MempoolAuthRpc *, x23::Async>);

} // namespace hogd
