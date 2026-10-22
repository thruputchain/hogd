#pragma once

#include <chrono>
#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <sys/types.h>
#include <thread>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/http/http.h"
#include "hogd/metrics/health.h"
#include "hogd/metrics/metrics.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/progname.h"

namespace hogd {

// Clamps a byte or item count to the signed range a gauge records.
inline ssize_t
saturating_gauge_value(size_t value)
{
  const auto max = static_cast<size_t>(std::numeric_limits<ssize_t>::max());
  if (value > max) {
    return std::numeric_limits<ssize_t>::max();
  }
  return static_cast<ssize_t>(value);
}

// Registers `{component}_resident_bytes`. Call before mark_done_with_init.
void register_resident_bytes(Metrics::ComponentName component_name);

// Fills every registered `{component}_resident_bytes` gauge from this
// process's RSS. The /metrics handler calls it at scrape time.
void sample_resident_bytes();

inline void
add_health_metrics_routes(HttpServer &server)
{
  server.add_route("/health", []() -> x23::Async<HttpResponse> {
    co_return HttpResponse{
        .status = 200,
        .content_type = "application/json",
        .body = co_await Health::instance().dump_to_string(),
    };
  });
  server.add_route("/metrics", [] {
    sample_resident_bytes();
    return HttpResponse{
        .status = 200,
        .content_type = "text/plain",
        .body = Metrics::instance().dump_to_prometheus_string(),
    };
  });
}

} // namespace hogd
