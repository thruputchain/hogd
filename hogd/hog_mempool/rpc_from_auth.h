#pragma once

#include <mutex>
#include <string_view>
#include <utility>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/version.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class MempoolAuthRpc {
public:
  using rpc_service = MempoolAuth1;
  using auth_data_callback =
      std::function<x23::Async<void>(BlockHeight, KeyChangeBatch)>;

  MempoolAuthRpc() = default;

  void register_callback(auth_data_callback &&callback)
  {
    const std::lock_guard lock(callback_mutex_);
    CHECK(!callback_.has_value());
    callback_ = std::move(callback);
  }

  static x23::Async<IntroAuthResult> mempoolauth_intro_auth(AuthCard card)
  {
    info("got {}, expected {}", card.protocol.c_str(),
         version::kHogdProtocol());
    if (std::string_view(card.protocol) !=
        std::string_view(version::kHogdProtocol())) {
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
    {
      const std::lock_guard lock(callback_mutex_);
      if (!callback_.has_value()) {
        warn("Ignoring auth-data update for height {} before mempool "
             "coordinator initialization",
             height);
        co_return;
      }
      callback = callback_;
    }
    co_await (*callback)(height, std::move(key_changes));
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
  std::mutex callback_mutex_;
  std::optional<auth_data_callback> callback_;
};

static_assert(is_MempoolAuth1<MempoolAuthRpc *, x23::Async>);

} // namespace hogd
