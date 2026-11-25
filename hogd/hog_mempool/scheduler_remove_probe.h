#pragma once

// Temporary commit diagnostics. No new synchronization: one record per
// existing worker chunk, read only after the original joins complete.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include <sys/resource.h>

namespace hogd {

struct SchedulerRemoveProbe {
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;
  struct Chunk {
    Time queued, started, finished;
    uint64_t cpu_begin = 0, cpu_end = 0;
    uint64_t voluntary = 0, involuntary = 0;
    uint64_t samples = 0, body_lookup_ns = 0, account_lookup_ns = 0;
    uint64_t runqueue_ns = 0, cache_erase_ns = 0;
  };
  Time entry, bg_end, lock_begin, lock_end, clear_end, launch_end;
  Time join_end, collect_end, ready_end, return_end;
  int64_t pending_before = 0, pending_after = 0;
  size_t cache_before = 0, cache_after = 0;
  size_t removed_from_runqueue = 0, affected_accounts = 0, expired = 0;
  std::vector<Chunk> chunks;

  static uint64_t duration(Time a, Time b)
  {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
  }
  static uint64_t thread_cpu_ns()
  {
#ifdef CLOCK_THREAD_CPUTIME_ID
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
      return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
             static_cast<uint64_t>(ts.tv_nsec);
    }
#endif
    return 0;
  }
  static void usage(uint64_t &voluntary, uint64_t &involuntary)
  {
#ifdef RUSAGE_THREAD
    rusage ru{};
    if (getrusage(RUSAGE_THREAD, &ru) == 0) {
      voluntary = static_cast<uint64_t>(ru.ru_nvcsw);
      involuntary = static_cast<uint64_t>(ru.ru_nivcsw);
    }
#endif
  }
  std::string receipt(uint64_t height, uint64_t block_id, size_t txs) const;
};

} // namespace hogd
