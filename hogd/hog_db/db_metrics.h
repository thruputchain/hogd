#pragma once

#include <array>
#include <ctime>
#include <string>
#include <string_view>

#include "hogd/base/types.h"
#include "hogd/metrics/metrics.h"
#include "x23/fastclock.h"

namespace hogd {

// Where hog_db's executor-facing handlers spend their time.
//
// x23's reader coroutine runs a synchronous handler inline, so a handler's
// wall time is also time its connection reads nothing else.  Wall time well
// above thread CPU time means the handler blocked (on a lock, say), and every
// request queued behind it on that connection waited too; summed over every
// handler, wall time per second of elapsed time is how many connection
// readers were occupied.  Sums in nanoseconds; divide by the count for means.
enum class DbRpcKind : uint8_t {
  kGetSnapshotItem,
  kTryPushSubtransaction,
  kCommitTransaction,
  kOther,
  kCount
};

inline constexpr std::array<std::string_view,
                            static_cast<size_t>(DbRpcKind::kCount)>
    kDbRpcKindNames = {"get_snapshot_item", "try_push_subtransaction",
                       "commit_transaction", "other"};

struct DbRpcMetrics {
  Metrics::Counter count;
  Metrics::Counter wall_ns;
  Metrics::Counter cpu_ns;
};

inline std::array<DbRpcMetrics, static_cast<size_t>(DbRpcKind::kCount)>
    db_rpc_metrics;

inline void
init_hog_db_metrics()
{
  Metrics &metrics = Metrics::instance();
  for (size_t i = 0; i < db_rpc_metrics.size(); i++) {
    const std::string name(kDbRpcKindNames[i]);
    db_rpc_metrics[i] = DbRpcMetrics{
        .count = metrics.per_thread_counter(Metrics::ComponentName::kDb,
                                            "rpc_" + name + "_count",
                                            "executor RPCs served"),
        .wall_ns =
            metrics.per_thread_counter(Metrics::ComponentName::kDb,
                                       "rpc_" + name + "_wall_ns",
                                       "total ns of wall time in the handler"),
        .cpu_ns = metrics.per_thread_counter(
            Metrics::ComponentName::kDb, "rpc_" + name + "_cpu_ns",
            "total ns of thread CPU time in the handler"),
    };
  }
  metrics.mark_done_with_init(Metrics::ComponentName::kDb);
}

inline uint64_t
thread_cpu_ns()
{
  timespec ts{};
  // NOLINTNEXTLINE(misc-include-cleaner): glibc defines it internally
  ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000) +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Times one handler call into db_rpc_metrics.  A no-op until
// init_hog_db_metrics() has run, as in tests that call handlers directly.
class DbRpcTimer {
public:
  explicit DbRpcTimer(DbRpcKind kind)
    : metrics_(db_rpc_metrics[static_cast<size_t>(kind)]),
      enabled_(static_cast<bool>(metrics_.count))
  {
    if (enabled_) {
      wall_start_ns_ = x23::fastclock::get_ns();
      cpu_start_ns_ = thread_cpu_ns();
    }
  }

  ~DbRpcTimer()
  {
    if (!enabled_) {
      return;
    }
    try {
      metrics_.count.increment();
      metrics_.wall_ns.increment(x23::fastclock::get_ns() - wall_start_ns_);
      metrics_.cpu_ns.increment(thread_cpu_ns() - cpu_start_ns_);
    } catch (...) { // NOLINT(bugprone-empty-catch): timing is best-effort
    }
  }

  DbRpcTimer(const DbRpcTimer &) = delete;
  DbRpcTimer &operator=(const DbRpcTimer &) = delete;
  DbRpcTimer(DbRpcTimer &&) = delete;
  DbRpcTimer &operator=(DbRpcTimer &&) = delete;

private:
  DbRpcMetrics &metrics_;
  bool enabled_;
  uint64_t wall_start_ns_ = 0;
  uint64_t cpu_start_ns_ = 0;
};

} // namespace hogd
