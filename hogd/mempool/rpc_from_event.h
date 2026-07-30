#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/shared.h"
#include "hogd/version.h"
#include "rpc_types.hh"
#include "sched_event.hh"
#include "x23/async.h"

namespace hogd {

class MempoolEventRpc {
public:
  using rpc_service = SchedEvent2;

  MempoolEventRpc() = default;

  // Disable copy and move.
  MempoolEventRpc(const MempoolEventRpc &) = delete;
  MempoolEventRpc &operator=(const MempoolEventRpc &) = delete;
  MempoolEventRpc(MempoolEventRpc &&) = delete;
  MempoolEventRpc &operator=(MempoolEventRpc &&) = delete;

  static EventPrepareResult schedevent2_prepare_event(const EventCard &card)
  {
    info("event server preparing registration");
    if (std::string_view(card.protocol) != version::kHogdProtocol()) {
      return EventPrepareResult{EventPrepareOutcome::EventPrepareError};
    }
    return Registration::registration().prepare_event(card);
  }

  static EventActivationResult
  schedevent2_activate_event(const EventRegistrationSnapshot &snapshot)
  {
    return Registration::registration().activate_event(snapshot);
  }

  static x23::Async<event_sched_client_t> get_client()
  {
    EventCard card;
    CHECK(Registration::registration().event_registered(&card));

    auto ret = co_await create_client<EventSched2>(
        server_address_to_sockaddr(card.address_for_sched), /*retry=*/false,
        kEventSchedulerRpcMaxMessageSize);
    if (!ret)
      panic("rpc_from_event get_client connect failed: {}",
            ret.error().error_code().message());
    co_return std::move(*ret);
  }

  static EventCard wait_for_registration(
      std::chrono::milliseconds timeout = std::chrono::seconds(30))
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool saw_canonical_tip = false;
    EventCard card;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto tip = Registration::registration().event_canonical_tip();
      saw_canonical_tip =
          tip && tip->consensus_value.proposal_header.block_height >= 0;
      if (saw_canonical_tip &&
          Registration::registration().event_registered(&card)) {
        return card;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!saw_canonical_tip) {
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

static_assert(is_SchedEvent2<MempoolEventRpc *, x23::MaybeAsync>);

} // namespace hogd
