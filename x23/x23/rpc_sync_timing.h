#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace x23::detail {

// Opt-in observation of one synchronous RPC procedure. The stream owns this
// object while dispatching. A handler may attach a sample for the observer.
struct RpcSyncTiming {
  uint64_t received_ns{};
  uint64_t handler_begin_ns{};
  uint64_t handler_end_ns{};
  uint64_t reply_attempt_ns{};
  std::array<uint64_t, 3> sample{};
};

struct RpcSyncTimingObserver {
  uint32_t program;
  uint32_t version;
  uint32_t procedure;
  void (*record)(const RpcSyncTiming &) noexcept;
};

inline std::atomic<const RpcSyncTimingObserver *> rpc_sync_timing_observer{};
inline thread_local RpcSyncTiming *rpc_sync_timing{};

} // namespace x23::detail
