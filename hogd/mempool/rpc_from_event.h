#pragma once

#include <atomic>
#include <cassert>

#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/shared.h"
#include "rpc_types.hh"
#include "sched_event.hh"
#include "x23/async.h"

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

  static x23::Async<event_sched_client_t> get_client()
  {
    EventCard card;
    CHECK(Registration::registration().event_registered(&card));

    auto ret = co_await create_client<EventSched1>(
        server_address_to_sockaddr(card.address_for_sched));
    if (!ret)
      panic("rpc_from_event get_client connect failed: {}",
            ret.error().error_code().message());
    co_return std::move(*ret);
  }

  void shut_down()
  {
    // TODO(jhumphri): Implement.
    shutdown_flag_ = true;
  }

private:
  std::atomic<bool> shutdown_flag_ = false;
};

static_assert(is_SchedEvent1<MempoolEventRpc *, x23::MaybeAsync>);

} // namespace hogd
