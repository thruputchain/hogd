#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace x23 {

// A diagnostic tag is copied with a queued record, without retaining a
// coroutine, buffer view, or pointer into a request handler.
struct RpcQueueTiming {
  uint64_t block{};
  uint64_t transaction{};
  uint64_t phase{};
  uint64_t received{};
  uint64_t handler_begin{};
  uint64_t handler_end{};
  uint64_t enqueued{};
  uint64_t dequeued{};
  uint64_t write_attempt{};
  void (*report)(const RpcQueueTiming &) noexcept {};
};

inline uint64_t
rpc_queue_now() noexcept
{
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
      .count();
}

struct RpcQueueServerProbe {
  uint32_t program;
  uint32_t version;
  uint32_t push;
  uint32_t commit;
};

inline std::atomic<const RpcQueueServerProbe *> rpc_queue_server_probe{};
inline thread_local RpcQueueTiming *rpc_queue_server_timing{};

} // namespace x23
