#pragma once

#include <string_view>
#include <utility>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/shared.h"
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
    CHECK(!callback_.has_value());
    callback_ = std::move(callback);
  }

  static x23::Async<IntroAuthResult> mempoolauth_intro_auth(AuthCard card)
  {
    info("got {}, expected {}", card.protocol.c_str(),
         version::kHogdProtocol());
    if (std::string_view(card.protocol) != std::string_view(version::kHogdProtocol())) {
      co_return IntroAuthResult(IntroAuthOutcome::kFailure);
    }

    Registration::registration().register_auth(card);
    x23::XdrVec<ShardCard> shard_cards;
    Registration::registration().shards_registered(&shard_cards);
    IntroAuthResult result(IntroAuthOutcome::kSuccess);
    result.shards() = shard_cards;
    co_return result;
  }

  x23::Async<void> mempoolauth_auth_data_changed(BlockHeight height,
                                                 KeyChangeBatch key_changes)
  {
    CHECK(callback_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    co_await (*callback_)(height, std::move(key_changes));
  }

  static x23::Async<auth_sched_client_t> get_client()
  {
    AuthCard card;
    CHECK(Registration::registration().auth_registered(&card));

    co_return co_await create_client<AuthMempool1>(
        server_address_to_sockaddr(card.address_for_mempool), /*retry=*/false,
        /*max_message_size=*/LoadgenRpc::kRpcMaxMessageSize);
  }

private:
  std::optional<auth_data_callback> callback_;
};

static_assert(is_MempoolAuth1<MempoolAuthRpc *, x23::Async>);

} // namespace hogd
