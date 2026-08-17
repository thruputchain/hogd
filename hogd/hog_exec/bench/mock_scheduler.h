#pragma once

#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/hog_exec/bench/control_client.h"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class MockSchedEvent {
public:
  using rpc_service = SchedEvent1;

  MockSchedEvent() = default;

  // Disable copy and move.
  MockSchedEvent(const MockSchedEvent &) = delete;
  MockSchedEvent &operator=(const MockSchedEvent &) = delete;
  MockSchedEvent(MockSchedEvent &&) = delete;
  MockSchedEvent &operator=(MockSchedEvent &&) = delete;

  EventPrepareResult schedevent_prepare_event(const EventCard &card)
  {
    std::unique_lock lk(mtx_);
    event_card_ = std::move(card);
    cv_.notify_all();
    EventPrepareResult result{EventPrepareOutcome::EventPrepareOK};
    result.snapshot().tip.block_id = std::numeric_limits<BlockID>::max();
    result.snapshot().tip.consensus_value.proposal_header.block_height = -1;
    result.snapshot().token = 1;
    return result;
  }

  static EventActivationResult
  schedevent_activate_event(const EventRegistrationSnapshot & /*unused*/)
  {
    return EventActivationResult{EventActivationOutcome::EventActivationOK};
  }

  EventCard await_event_card()
  {
    std::unique_lock lk(mtx_);
    cv_.wait(lk, [this] { return event_card_.has_value(); });
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return event_card_.value();
  }

private:
  std::mutex mtx_;
  std::condition_variable cv_;
  std::optional<EventCard> event_card_;
};

static_assert(is_SchedEvent1<MockSchedEvent *, std::type_identity_t>);

class MockSchedDB {
public:
  using rpc_service = SchedDb1;

  MockSchedDB() = default;

  // Disable copy and move.
  MockSchedDB(const MockSchedDB &) = delete;
  MockSchedDB &operator=(const MockSchedDB &) = delete;
  MockSchedDB(MockSchedDB &&) = delete;
  MockSchedDB &operator=(MockSchedDB &&) = delete;

  void scheddb_intro_shard(const ShardCard &card)
  {
    std::unique_lock lk(mtx_);
    shard_cards_.push_back(card);
    cv_.notify_all();
  }

  static void scheddb_hogs_transferred(const HogsTransferred &hogs) {}

  x23::Vec<ShardCard> await_shard_cards(unsigned int num_cards)
  {
    std::unique_lock lk(mtx_);
    cv_.wait(lk,
             [this, num_cards] { return shard_cards_.size() == num_cards; });
    return shard_cards_;
  }

private:
  std::mutex mtx_;
  std::condition_variable cv_;
  x23::Vec<ShardCard> shard_cards_;
};

static_assert(is_SchedDb1<MockSchedDB *, std::type_identity_t>);

class ControlServerHandle {
public:
  ControlServerHandle(std::string const &server_addr)
    : ci_client(connect_to_control_server(server_addr,
                                          std::to_string(kControlServerPort)))
  {}

  ControlClientWrapper ci_client;

  ~ControlServerHandle()
  {
    try {
      ci_client.stop_server();
    } catch (...) {
      panic("Failed to stop control server");
    }
  }
};

} // namespace hogd
