#pragma once

#include <iostream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/merkle/hex_array.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "sched_exec.hh"
#include "transaction.hh"
#include "types.h"
#include "types.hh"
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

  void wait_for_registration(size_t num_executors, size_t num_shards) const
  {
    wait_for([this] { return coordinator_registered(); });
    wait_for([this, num_executors] {
      return num_executors < num_executors_registered();
    });
    wait_for([this] { return event_registered(); });
    wait_for(
        [this, num_shards] { return num_shards < num_shards_registered(); });
  }

  // Coordinator.
  void register_coordinator(const CoordCard &card)
  {
    coord_card_ = card;
    coordinator_registered_.store(true, std::memory_order_release);
  }
  bool coordinator_registered() const
  {
    return coordinator_registered_.load(std::memory_order_acquire);
  }

  // Executors.
  void register_executor(const ExecCard &card)
  {
    exec_cards_.push_back(card);
    num_executors_registered_.fetch_add(1, std::memory_order_release);
  }
  size_t num_executors_registered() const
  {
    return num_executors_registered_.load(std::memory_order_acquire);
  }

  // Event.
  void register_event(const EventCard &card)
  {
    event_card_ = card;
    event_registered_.store(true, std::memory_order_release);
  }
  bool event_registered() const
  {
    return event_registered_.load(std::memory_order_acquire);
  }

  // Databases.
  void register_shard(const ShardCard &card)
  {
    shard_cards_.push_back(card);
    num_shards_registered_.fetch_add(1, std::memory_order_release);
  }
  size_t num_shards_registered() const
  {
    return num_shards_registered_.load(std::memory_order_acquire);
  }

private:
  // No one should construct a [Registration] instance directly. Instead, use
  // the shared instance returned by [registration()]. There is a single shared
  // registration instance for the entire process.
  Registration() = default;
  ~Registration() = default;

  void wait_for(const std::function<bool(void)> &cond) const
  {
    while (!cond()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  std::atomic<bool> coordinator_registered_ = false;
  CoordCard coord_card_;

  std::atomic<size_t> num_executors_registered_ = 0;
  x23::Vec<ExecCard> exec_cards_;

  std::atomic<bool> event_registered_ = false;
  EventCard event_card_;

  std::atomic<size_t> num_shards_registered_ = 0;
  x23::Vec<ShardCard> shard_cards_;
};

} // namespace hogd
