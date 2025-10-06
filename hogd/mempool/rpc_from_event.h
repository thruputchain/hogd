#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

#include "hogd/base/log.h"
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
    return event_.value();
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<EventCard> event_;
};

static_assert(is_SchedEvent1<MempoolEventRpc *, x23::MaybeAsync>);

} // namespace hogd
