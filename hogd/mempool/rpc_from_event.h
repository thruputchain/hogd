#pragma once

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
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

  RPCError schedevent_intro_event(EventCard card)
  {
    info("event server connected");

    std::lock_guard lk(mutex_);
    if (event_.has_value()) {
      error("event server already connected");
      return RPCError::kRPCUnrecoverable;
    }
    event_ = std::move(card);
    cv_.notify_all();
    return RPCError::kRPCNone;
  }

  EventCard event()
  {
    std::unique_lock lk(mutex_);
    cv_.wait(lk, [this] { return event_.has_value(); });
    assert(event_.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return event_.value();
  }

  auto get_client(x23::CoroPoll &cp)
  {
    assert(event_.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    x23::UniqueFd fd =
        x23::tcpconnect(server_address_to_sockaddr(event_.value().address));
    // NOLINTEND(bugprone-unchecked-optional-access)
    return x23::rpc_stream_logged(cp, fd.release())->client(EventSched1{});
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<EventCard> event_;
};

static_assert(is_SchedEvent1<MempoolEventRpc *, x23::MaybeAsync>);

} // namespace hogd
