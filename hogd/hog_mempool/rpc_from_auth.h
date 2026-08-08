#pragma once

#include <optional>
#include <string_view>
#include <sys/socket.h>
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

    // Reject unusable reverse-callback addresses before publishing the card.
    // The resolved address is stored and reused by [get_client]; without this,
    // a malformed or unresolvable [address_for_mempool] would abort the mempool
    // during coordinator-driven [finish_init]. Resolving once here (rather than
    // re-resolving the string later under a fatal CHECK) also closes the window
    // where a hostname that resolves now fails to resolve at [get_client] time.
    std::optional<sockaddr_storage> mempool_addr =
        try_server_address_to_sockaddr(card.address_for_mempool);
    if (!mempool_addr) {
      info("rejecting auth intro: unusable address_for_mempool {}",
           card.address_for_mempool.c_str());
      co_return IntroAuthResult(IntroAuthOutcome::kFailure);
    }

    Registration::registration().register_auth(card, *mempool_addr);
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
    sockaddr_storage mempool_addr;
    CHECK(Registration::registration().auth_registered(/*card=*/nullptr,
                                                       &mempool_addr));

    auto ret = co_await create_client<AuthMempool1>(
        mempool_addr, /*retry=*/false,
        /*max_message_size=*/kBulkRpcMaxMessageSize);
    if (!ret)
      panic("rpc_from_auth get_client connect failed: {}",
            ret.error().error_code().message());
    co_return std::move(*ret);
  }

private:
  std::optional<auth_data_callback> callback_;
};

static_assert(is_MempoolAuth1<MempoolAuthRpc *, x23::Async>);

} // namespace hogd
