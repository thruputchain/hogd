#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/version.h"
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

  static EventPrepareResult schedevent_prepare_event(const EventCard &card)
  {
    info("event server preparing registration");
    if (std::string_view(card.protocol) != version::kHogdProtocol()) {
      return EventPrepareResult{EventPrepareOutcome::EventPrepareError};
    }
    return Registration::registration().prepare_event(card);
  }

  static EventActivationResult
  schedevent_activate_event(const EventRegistrationSnapshot &snapshot)
  {
    return Registration::registration().activate_event(snapshot);
  }

  static x23::Async<event_sched_client_t> get_client()
  {
    EventCard card;
    CHECK(Registration::registration().event_registered(&card));

    auto ret = co_await create_client<EventSched1>(
        server_address_to_sockaddr(card.address_for_sched), /*retry=*/false,
        kEventSchedulerRpcMaxMessageSize);
    if (!ret)
      panic("rpc_from_event get_client connect failed: {}",
            ret.error().error_code().message());
    co_return std::move(*ret);
  }

  // Suspends -- without occupying a thread-pool worker -- until the event has
  // activated and published a canonical tip. See [Registration::event_ready()].
  static x23::Async<EventCard> wait_for_registration()
  {
    Registration &reg = Registration::registration();
    co_await reg.wait_for_event_ready();
    // Readiness is only published while the event is registered, so the card
    // is set by the time we get here.
    EventCard card;
    reg.event_registered(&card);
    co_return card;
  }

  static EventCard wait_for_registration_for(std::chrono::milliseconds timeout)
  {
    Registration &reg = Registration::registration();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    EventCard card;
    while (std::chrono::steady_clock::now() < deadline) {
      if (reg.event_ready() && reg.event_registered(&card)) {
        return card;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto tip = reg.event_canonical_tip();
    if (!tip || tip->consensus_value.proposal_header.block_height < 0) {
      panic("event startup timed out waiting for canonical tip");
    }
    panic("event startup timed out waiting for activation");
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
