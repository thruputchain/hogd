#pragma once

#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/event/event_block_context.h"
#include "hogd/event/rpc_context.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "x23/async.h"
#include <string_view>
#include <utility>

namespace hogd {

class EventIndexerRpc {
public:
  using rpc_service = EventIndexer1;

  EventIndexerRpc(EventRPCContext &ctx) : ctx_(ctx) {}

  // TODO(jhumphri): Should the indexer instead register with the mempool, which
  // in turn will send the indexer's address to the event server?
  x23::Async<EventIndexerIntroOutcome> event_indexer_intro(IndexerCard card)
  {
    // The protocol string is remote RPC input, not an internal invariant: a
    // peer speaking a different protocol must be rejected without aborting the
    // event server. Reject before dialing the peer's advertised address.
    if (std::string_view(card.protocol) !=
        std::string_view(version::kHogdProtocol())) {
      warn("event_indexer_intro: rejecting indexer with mismatched protocol "
           "(got {}, expected {})",
           std::string_view(card.protocol),
           std::string_view(version::kHogdProtocol()));
      co_return EventIndexerIntroOutcome::EventIntroMismatchedProtocol;
    }
    auto client = co_await make_rpc_client<IndexerEvent1>(
        server_address_to_sockaddr(card.address_for_event), /*retry=*/false,
        /*max_message_size=*/LoadgenRpc::kRpcMaxMessageSize);
    if (!client) {
      // The dial back to the indexer's advertised address failed. Report it so
      // the indexer fails startup instead of treating registration as
      // successful while we hold no client for it.
      warn("event_indexer_intro: failed to connect to indexer {}: {}",
           std::string_view(card.address_for_event),
           client.error().error_code().message());
      co_return EventIndexerIntroOutcome::EventIntroConnectFailed;
    }
    ctx_.add_indexer(std::move(*client));
    co_return EventIndexerIntroOutcome::EventIntroSuccess;
  }

private:
  EventRPCContext &ctx_;
};
static_assert(is_EventIndexer1<EventIndexerRpc *, x23::MaybeAsync>);

class EventIndexerServer : public RpcServer<EventIndexerRpc> {
public:
  explicit EventIndexerServer(EventRPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
