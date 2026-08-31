#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <utility>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/version.h"
#include "hogd/xdr/protocol_token.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/x23/corosync.h"

namespace hogd {

class MempoolAuthRpc {
public:
  using rpc_service = MempoolAuth1;
  using auth_data_callback =
      std::function<x23::Async<void>(BlockHeight, KeyChangeBatch)>;

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
    while (true) {
      if (callbacks_in_flight_) {
        cg.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        co_await cg(callback_lock_);
        continue;
      }
      break;
    }
  }

  static x23::Async<IntroAuthResult> mempoolauth_intro_auth(AuthCard card)
  {
    info("got {}, expected {}", protocol_text(card.protocol),
         protocol_text(version::kHogdProtocol()));
    if (card.protocol != version::kHogdProtocol()) {
      co_return IntroAuthResult(IntroAuthOutcome::kFailure);
    }

    Registration::registration().register_auth(card);
    x23::XdrVec<ShardCard> shard_cards;
    Registration::registration().shards_registered(&shard_cards);
    NetworkHash network_hash{};
    Registration::registration().get_network_hash(&network_hash);
    IntroAuthResult result(IntroAuthOutcome::kSuccess);
    result.success() = MempoolAuthIntroSuccess{
        .shards = shard_cards,
        .network_hash = network_hash,
    };
    co_return result;
  }

  x23::Async<void> mempoolauth_auth_data_changed(BlockHeight height,
                                                 KeyChangeBatch key_changes)
  {
    std::optional<auth_data_callback> callback;
    while (true) {
      x23::CoroGuard cg;
      co_await cg(callback_lock_);

      if (!callback_) {
        info("Recording auth-data watermark {} before mempool coordinator "
             "initialization; discarding {} key changes for an empty "
             "scheduler",
             height, key_changes.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      callback = callback_;
      ++callbacks_in_flight_;
      break;
    }

    co_await (*callback)(height, std::move(key_changes));

    {
      x23::CoroGuard cg;
      co_await cg(callback_lock_);

      CHECK_GT(callbacks_in_flight_, size_t{0});
      --callbacks_in_flight_;
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
  x23::CoroLock callback_lock_;
  std::optional<auth_data_callback> callback_;
  size_t callbacks_in_flight_ = 0;
};

static_assert(is_MempoolAuth1<MempoolAuthRpc *, x23::Async>);

} // namespace hogd
