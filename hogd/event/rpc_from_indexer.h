#pragma once

#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/rpc_context.h"
#include "hogd/version.h"
#include "hogd/xdr/indexer_payload_validation.h"
#include "indexer.hh"
#include "x23/async.h"
#include <string_view>
#include <utility>

namespace hogd {

class EventIndexerRpc {
public:
  using rpc_service = EventIndexer2;

  EventIndexerRpc(EventRPCContext &ctx) : ctx_(ctx) {}

  x23::Async<EventIndexerRegistrationOutcome>
  event_indexer_register(IndexerCardV2 card)
  {
    // The protocol string is remote RPC input, not an internal invariant: a
    // peer speaking a different protocol must be rejected without aborting the
    // event server. Reject before dialing the peer's advertised address.
    if (std::string_view(card.protocol) !=
        std::string_view(version::kHogdProtocol())) {
      warn("event_indexer_register: rejecting indexer with mismatched protocol "
           "(got {}, expected {})",
           std::string_view(card.protocol),
           std::string_view(version::kHogdProtocol()));
      co_return EventIndexerRegistrationOutcome::
          EventRegistrationMismatchedProtocol;
    }
    x23::HostPort indexer_address;
    if (!indexer_address.parse(card.address_for_event)) {
      warn("event_indexer_register: invalid indexer address {}",
           std::string_view(card.address_for_event));
      co_return EventIndexerRegistrationOutcome::EventRegistrationConnectFailed;
    }
    auto resolved = indexer_address.to_addr(/*dns=*/true);
    if (!resolved) {
      warn("event_indexer_register: failed to resolve indexer {}: {}",
           std::string_view(card.address_for_event),
           resolved.error().error_code().message());
      co_return EventIndexerRegistrationOutcome::EventRegistrationConnectFailed;
    }
    auto client = co_await make_rpc_client<IndexerEvent2>(
        *resolved, /*retry=*/false,
        /*max_message_size=*/kIndexerRpcMaxMessageSize);
    if (!client) {
      // The dial back to the indexer's advertised address failed. Report it so
      // the indexer fails startup instead of treating registration as
      // successful while we hold no client for it.
      warn("event_indexer_register: failed to connect to indexer {}: {}",
           std::string_view(card.address_for_event),
           client.error().error_code().message());
      co_return EventIndexerRegistrationOutcome::EventRegistrationConnectFailed;
    }
    co_return co_await ctx_.register_indexer(std::move(*client), card);
  }

private:
  EventRPCContext &ctx_;
};
static_assert(is_EventIndexer2<EventIndexerRpc *, x23::MaybeAsync>);

class EventIndexerServer : public RpcServer<EventIndexerRpc> {
public:
  explicit EventIndexerServer(EventRPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
