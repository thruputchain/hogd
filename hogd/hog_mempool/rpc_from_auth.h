#pragma once

#include <expected>
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
#include "x23/sockio.h"

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

    // Preflight the advertised callback before publishing the registration.
    // [get_client] later resolves and dials this address with retry disabled
    // and panics on failure, so a malformed, unresolvable, or unreachable
    // [address_for_mempool] would abort coordinator initialization. Reject such
    // a card here instead of storing a callback that cannot be reached. Parse
    // defensively: [server_address_to_sockaddr] CHECK-fails on bad input, which
    // would itself be an abort triggered by the caller-supplied address.
    x23::HostPort callback;
    if (!callback.parse(std::string_view(card.address_for_mempool))) {
      info("mempoolauth_intro_auth rejecting card with unparseable callback {}",
           card.address_for_mempool.c_str());
      co_return IntroAuthResult(IntroAuthOutcome::kFailure);
    }
    std::expected<sockaddr_storage, x23::GaiError> callback_addr =
        callback.to_addr(/*dns=*/true);
    if (!callback_addr) {
      info("mempoolauth_intro_auth rejecting card with unresolvable callback "
           "{}",
           card.address_for_mempool.c_str());
      co_return IntroAuthResult(IntroAuthOutcome::kFailure);
    }
    auto preflight = co_await create_client<AuthMempool1>(
        *callback_addr, /*retry=*/false,
        /*max_message_size=*/kBulkRpcMaxMessageSize);
    if (!preflight) {
      info("mempoolauth_intro_auth rejecting card with unreachable callback "
           "{}: {}",
           card.address_for_mempool.c_str(),
           preflight.error().error_code().message());
      co_return IntroAuthResult(IntroAuthOutcome::kFailure);
    }

    if (!Registration::registration().register_auth(card)) {
      // An auth card is already registered; do not overwrite it.
      co_return IntroAuthResult(IntroAuthOutcome::kFailure);
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

    auto ret = co_await create_client<AuthMempool1>(
        server_address_to_sockaddr(card.address_for_mempool), /*retry=*/false,
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
