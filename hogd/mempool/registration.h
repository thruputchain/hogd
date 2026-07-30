#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>

#include "auth_card.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "sched_coord.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "sched_exec.hh"
#include "x23/vec.h"

namespace hogd {

class Registration {
public:
  Registration(const Registration &) = delete;
  Registration &operator=(const Registration &) = delete;
  Registration(Registration &&) = delete;
  Registration &operator=(Registration &&) = delete;

  // Do not allow heap allocation. (See comment below in private section below
  // for [Registration] constructor).
  void *operator new(std::size_t) = delete;
  void operator delete(void *) = delete;

  // There is a single shared registration instance for the entire process.
  static Registration &registration()
  {
    static Registration registration;
    return registration;
  }

  // Coordinator.
  void register_coordinator(const CoordCard &card)
  {
    coord_card_ = card;
    coordinator_registered_.store(true, std::memory_order_release);
  }
  bool coordinator_registered(CoordCard *card) const
  {
    bool ret = coordinator_registered_.load(std::memory_order_acquire);
    if (card) {
      *card = coord_card_;
    }
    return ret;
  }

  // Executors.
  // Returns the ID of the registered executor.
  size_t register_executor(const ExecCard &card)
  {
    std::unique_lock lk(exec_cards_mutex_);
    exec_cards_.push_back(card);
    return exec_cards_.size() - 1;
  }
  size_t executors_registered(x23::Vec<ExecCard> *cards)
  {
    std::unique_lock lk(exec_cards_mutex_);
    if (cards) {
      *cards = exec_cards_;
    }
    return exec_cards_.size();
  }

  // Event.
  EventPrepareResult prepare_event(const EventCard &card)
  {
    std::unique_lock lk(event_mutex_);
    if (event_registered_.load(std::memory_order_acquire) ||
        pending_event_card_) {
      return EventPrepareResult{EventPrepareOutcome::EventPrepareError};
    }
    if (!event_canonical_tip_) {
      return EventPrepareResult{EventPrepareOutcome::EventPrepareNotReady};
    }
    const BlockHeight tip_height =
        event_canonical_tip_->consensus_value.proposal_header.block_height;
    if (tip_height == -1) {
      return EventPrepareResult{EventPrepareOutcome::EventPrepareNotReady};
    }
    if (tip_height < -1) {
      return EventPrepareResult{EventPrepareOutcome::EventPrepareError};
    }
    pending_event_card_ = card;
    pending_event_tip_ = *event_canonical_tip_;
    EventPrepareResult result{EventPrepareOutcome::EventPrepareOK};
    result.snapshot().tip = *pending_event_tip_;
    result.snapshot().token = ++next_event_registration_token_;
    pending_event_token_ = result.snapshot().token;
    return result;
  }
  EventActivationResult
  activate_event(const EventRegistrationSnapshot &snapshot)
  {
    std::unique_lock lk(event_mutex_);
    if (!event_canonical_tip_ || !pending_event_card_ || !pending_event_tip_ ||
        pending_event_token_ != snapshot.token) {
      return EventActivationResult{
          EventActivationOutcome::EventActivationError};
    }
    if (snapshot.tip != *pending_event_tip_) {
      return EventActivationResult{
          EventActivationOutcome::EventActivationError};
    }
    if (*pending_event_tip_ != *event_canonical_tip_) {
      EventActivationResult result{
          EventActivationOutcome::EventActivationRetry};
      result.current_tip() = *event_canonical_tip_;
      pending_event_card_.reset();
      pending_event_tip_.reset();
      pending_event_token_ = 0;
      return result;
    }
    event_card_ = std::move(*pending_event_card_);
    pending_event_card_.reset();
    pending_event_tip_.reset();
    pending_event_token_ = 0;
    event_registered_.store(true, std::memory_order_release);
    return EventActivationResult{EventActivationOutcome::EventActivationOK};
  }
  bool event_registered(EventCard *card) const
  {
    std::unique_lock lk(event_mutex_);
    const bool ret = event_registered_.load(std::memory_order_acquire);
    if (ret && card) {
      *card = event_card_;
    }
    return ret;
  }
  void publish_event_canonical_tip(EventCanonicalTip tip)
  {
    std::unique_lock lk(event_mutex_);
    event_canonical_tip_ = std::move(tip);
  }
  std::optional<EventCanonicalTip> event_canonical_tip() const
  {
    std::unique_lock lk(event_mutex_);
    return event_canonical_tip_;
  }

  // Auth.
  void register_auth(const AuthCard &card)
  {
    auth_card_ = card;
    auth_registered_.store(true, std::memory_order_release);
  }
  bool auth_registered(AuthCard *card) const
  {
    bool ret = auth_registered_.load(std::memory_order_acquire);
    if (card) {
      *card = auth_card_;
    }
    return ret;
  }

  // Databases.
  // Returns the ID of the registered shard.
  size_t register_shard(const ShardCard &card)
  {
    std::unique_lock lk(shard_cards_mutex_);
    shard_cards_.push_back(card);
    return shard_cards_.size() - 1;
  }
  size_t shards_registered(x23::XdrVec<ShardCard> *cards)
  {
    std::unique_lock lk(shard_cards_mutex_);
    if (cards) {
      *cards = shard_cards_;
    }
    return shard_cards_.size();
  }

  // There is one shared registration instance for the process. There is no
  // circumstance in which the registration should be cleared, apart from in
  // tests. No one should call this function except for tests.
  void clear()
  {
    coordinator_registered_ = false;
    coord_card_ = CoordCard{};
    {
      std::unique_lock lk(exec_cards_mutex_);
      exec_cards_.clear();
    }
    {
      std::unique_lock lk(event_mutex_);
      event_registered_ = false;
      event_card_ = EventCard{};
      pending_event_card_.reset();
      pending_event_tip_.reset();
      pending_event_token_ = 0;
      next_event_registration_token_ = 0;
      event_canonical_tip_.reset();
    }
    {
      std::unique_lock lk(shard_cards_mutex_);
      shard_cards_.clear();
    }
    auth_registered_ = false;
    auth_card_ = AuthCard{};
  }

private:
  // No one should construct a [Registration] instance directly. Instead, use
  // the shared instance returned by [registration()]. There is a single shared
  // registration instance for the entire process.
  Registration() = default;
  ~Registration() = default;

  std::atomic<bool> coordinator_registered_ = false;
  CoordCard coord_card_;

  std::mutex exec_cards_mutex_;
  x23::Vec<ExecCard> exec_cards_;

  std::atomic<bool> event_registered_ = false;
  mutable std::mutex event_mutex_;
  EventCard event_card_;
  std::optional<EventCard> pending_event_card_;
  std::optional<EventCanonicalTip> pending_event_tip_;
  uint64_t pending_event_token_ = 0;
  uint64_t next_event_registration_token_ = 0;
  std::optional<EventCanonicalTip> event_canonical_tip_;

  std::atomic<bool> auth_registered_ = false;
  AuthCard auth_card_;

  std::mutex shard_cards_mutex_;
  x23::Vec<ShardCard> shard_cards_;
};

} // namespace hogd
