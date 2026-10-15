#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <string_view>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/time.h"
#include "hogd/metrics/metrics.h"
#include "x23/async.h"
#include "x23/corosync.h"

namespace hogd {
class InstrumentedCoroGuard {
public:
  InstrumentedCoroGuard(Metrics::Histogram &wait, Metrics::Histogram &hold)
    : wait_(wait), hold_(hold)
  {}
  InstrumentedCoroGuard(const InstrumentedCoroGuard &) = delete;
  InstrumentedCoroGuard &operator=(const InstrumentedCoroGuard &) = delete;
  ~InstrumentedCoroGuard() { reset(); }
  struct AwaitAcquire {
    decltype(std::declval<x23::CoroGuard &>()(
        std::declval<x23::CoroLock &>())) inner;
    InstrumentedCoroGuard &owner;
    std::optional<ElapsedTimer> timer;
    bool await_ready() noexcept
    {
      timer.emplace();
      return inner.await_ready();
    }
    void await_suspend(x23::CoroHandle handle) const noexcept
    {
      inner.await_suspend(handle);
    }
    void await_resume()
    {
      inner.await_resume();
      owner.held_.emplace();
      owner.wait_.record(timer->elapsed());
    }
  };
  // Forward the native awaiter instead of allocating an extra coroutine frame
  // on every lock acquisition.
  AwaitAcquire operator()(x23::CoroLock &lock)
  {
    return {.inner = guard_(lock), .owner = *this, .timer = std::nullopt};
  }
  void reset()
  {
    // Capture before unlock: a resumed coroutine may run before unlock returns.
    const double duration = held_ ? held_->elapsed() : 0;
    const bool record = held_.has_value();
    held_.reset();
    guard_.reset();
    if (record)
      hold_.record(duration);
  }

private:
  x23::CoroGuard guard_;
  Metrics::Histogram &wait_;
  Metrics::Histogram &hold_;
  std::optional<ElapsedTimer> held_;
};

// Intervals use the same monotonic clock on the mempool host. No cross-host
// clock synchronization is needed to establish submission/block overlap.
class IngestionInterval {
public:
  explicit IngestionInterval(std::string_view phase, bool sample = false)
    : phase_(phase), enabled_(true)
  {
    if (sample) {
      static std::atomic<uint64_t> sequence{0};
      enabled_ = sequence.fetch_add(1, std::memory_order_relaxed) % 64 == 0;
    }
    if (enabled_) {
      start_ = now();
      unix_start_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    }
  }
  ~IngestionInterval()
  {
    if (enabled_)
      info("INGESTION_INTERVAL phase={} start_ns={} end_ns={} unix_start_ns={}",
           phase_, start_, now(), unix_start_);
  }

private:
  static int64_t now()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  std::string_view phase_;
  bool enabled_;
  int64_t start_ = 0;
  int64_t unix_start_ = 0;
};
} // namespace hogd
