#pragma once

// Measurement-only RPC profiling: lock-free accumulators, reported as
// cumulative "RPCPROF" log lines every two seconds while they change.

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include "hogd/base/log.h"
#include "x23/rpc.h"

namespace hogd::rpc_profile {

// Count, total, maximum and a log2-of-microseconds histogram of durations.
struct Acc {
  static constexpr size_t kBuckets = 24;
  std::atomic<uint64_t> n{0};
  std::atomic<uint64_t> sum_ns{0};
  std::atomic<uint64_t> max_ns{0};
  std::atomic<uint64_t> bytes{0};
  std::array<std::atomic<uint64_t>, kBuckets> hist{};

  void add(uint64_t ns, uint64_t b = 0)
  {
    n.fetch_add(1, std::memory_order_relaxed);
    sum_ns.fetch_add(ns, std::memory_order_relaxed);
    bytes.fetch_add(b, std::memory_order_relaxed);
    uint64_t prev = max_ns.load(std::memory_order_relaxed);
    while (ns > prev &&
           !max_ns.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
    }
    const uint64_t us = ns / 1000;
    size_t bucket = us == 0 ? 0 : static_cast<size_t>(std::bit_width(us));
    if (bucket >= kBuckets) {
      bucket = kBuckets - 1;
    }
    hist[bucket].fetch_add(1, std::memory_order_relaxed);
  }

  std::string show() const
  {
    std::string h;
    for (size_t i = 0; i < kBuckets; i++) {
      const uint64_t c = hist[i].load(std::memory_order_relaxed);
      if (c != 0) {
        // Bucket i holds durations below 2^i microseconds.
        h += std::to_string(i) + ":" + std::to_string(c) + ",";
      }
    }
    return "n=" + std::to_string(n.load(std::memory_order_relaxed)) +
           " sum_ms=" +
           std::to_string(sum_ns.load(std::memory_order_relaxed) / 1000000) +
           " max_ms=" +
           std::to_string(max_ns.load(std::memory_order_relaxed) / 1000000) +
           " bytes=" + std::to_string(bytes.load(std::memory_order_relaxed)) +
           " hist_log2us=" + h;
  }
};

inline uint64_t
ns_between(std::chrono::steady_clock::time_point a,
           std::chrono::steady_clock::time_point b)
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

struct Profile {
  // Server side, on the connection reader (x23 hook).
  Acc body_read;
  Acc serve_sync;
  // Client side, encoding auth verify calls (x23 hook).
  Acc auth_encode;
  // hog-auth verify_transactions handler.
  Acc handler_queue;
  Acc handler_lookup;
  Acc handler_verify;
  Acc handler_total;
  std::atomic<uint64_t> handler_txs{0};
  std::atomic<int64_t> in_flight{0};
  std::atomic<int64_t> max_in_flight{0};
  std::atomic<uint64_t> in_flight_at_entry_sum{0};
};

inline Profile &
profile()
{
  static Profile p;
  return p;
}

inline void
on_serve(const x23::RpcServeSample &s)
{
  profile().body_read.add(s.body_read_ns, s.bytes);
  profile().serve_sync.add(s.serve_ns, s.bytes);
}

inline void
on_encode(const x23::RpcEncodeSample &s)
{
  if (std::strcmp(s.proc_name, "authmempool_verify_transactions") == 0) {
    profile().auth_encode.add(s.encode_ns, s.bytes);
  }
}

struct InFlight {
  InFlight()
  {
    auto &p = profile();
    const int64_t now = p.in_flight.fetch_add(1, std::memory_order_relaxed) + 1;
    p.in_flight_at_entry_sum.fetch_add(static_cast<uint64_t>(now),
                                       std::memory_order_relaxed);
    int64_t prev = p.max_in_flight.load(std::memory_order_relaxed);
    while (now > prev && !p.max_in_flight.compare_exchange_weak(
                             prev, now, std::memory_order_relaxed)) {
    }
  }
  ~InFlight() { profile().in_flight.fetch_sub(1, std::memory_order_relaxed); }
  InFlight(const InFlight &) = delete;
  InFlight &operator=(const InFlight &) = delete;
};

// Installs the x23 hooks and starts the reporter thread. Call once.
inline void
start(const char *component)
{
  x23::rpc_serve_hook.store(&on_serve);
  x23::rpc_encode_hook.store(&on_encode);
  std::thread([component] {
    uint64_t last = ~uint64_t{0};
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      auto &p = profile();
      const uint64_t seen = p.body_read.n.load() + p.auth_encode.n.load() +
                            p.handler_total.n.load();
      if (seen == last) {
        continue;
      }
      last = seen;
      const std::string body_read = p.body_read.show();
      const std::string serve_sync = p.serve_sync.show();
      const std::string auth_encode = p.auth_encode.show();
      const std::string queue = p.handler_queue.show();
      const std::string lookup = p.handler_lookup.show();
      const std::string verify = p.handler_verify.show();
      const std::string total = p.handler_total.show();
      const uint64_t txs = p.handler_txs.load();
      const int64_t max_in_flight = p.max_in_flight.load();
      const uint64_t entry_sum = p.in_flight_at_entry_sum.load();
      info("RPCPROF {} body_read {}", component, body_read);
      info("RPCPROF {} serve_sync {}", component, serve_sync);
      info("RPCPROF {} auth_encode {}", component, auth_encode);
      info("RPCPROF {} handler_queue {}", component, queue);
      info("RPCPROF {} handler_lookup {}", component, lookup);
      info("RPCPROF {} handler_verify {}", component, verify);
      info("RPCPROF {} handler_total {}", component, total);
      info("RPCPROF {} handler_txs={} max_in_flight={} in_flight_at_entry_sum={}",
           component, txs, max_in_flight, entry_sum);
    }
  }).detach();
}

} // namespace hogd::rpc_profile
