#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stop_token>

#include "hogd/base/rpc_client.h"
#include "sched_coord.hh"

namespace hogd {

// Development-only block production with immediate externalization. The caller
// owns the connection and thread; access counts only after run() has stopped.
class BlockDriver {
public:
  explicit BlockDriver(rpc_client_t<SchedulerCoord1> &scheduler)
    : scheduler_(scheduler)
  {}

  std::size_t produce_block();
  void run(std::chrono::steady_clock::time_point epoch,
           std::chrono::milliseconds interval, const std::stop_token &stop);

  std::uint64_t blocks() const { return blocks_; }
  std::uint64_t transactions() const { return transactions_; }

private:
  rpc_client_t<SchedulerCoord1> &scheduler_;
  std::uint64_t blocks_ = 0;
  std::uint64_t transactions_ = 0;
};

} // namespace hogd
