#pragma once

#include <cassert>
#include <condition_variable>
#include <optional>
#include <utility>

#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/mempool/registration.h"
#include "rpc_types.hh"
#include "sched_event.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

class MempoolEventRpc {
public:
  using rpc_service = SchedEvent1;

  MempoolEventRpc() = default;

  // Disable copy and move.
  MempoolEventRpc(const MempoolEventRpc &) = delete;
  MempoolEventRpc &operator=(const MempoolEventRpc &) = delete;
  MempoolEventRpc(MempoolEventRpc &&) = delete;
  MempoolEventRpc &operator=(MempoolEventRpc &&) = delete;

  static RPCError schedevent_intro_event(const EventCard &card)
  {
    info("event server connected");

    if (Registration::registration().event_registered(/*card=*/nullptr)) {
      error("event server already connected");
      return RPCError::kRPCUnrecoverable;
    }
    Registration::registration().register_event(card);
    return RPCError::kRPCNone;
  }

  static event_sched_client_t get_client(x23::CoroPoll &cp)
  {
    EventCard card;
    CHECK(Registration::registration().event_registered(&card));

    x23::UniqueFd fd =
        x23::tcpconnect(server_address_to_sockaddr(card.address_for_sched));
    return x23::rpc_stream_logged(cp, fd.release())->client(EventSched1{});
  }
};

static_assert(is_SchedEvent1<MempoolEventRpc *, x23::MaybeAsync>);

} // namespace hogd
