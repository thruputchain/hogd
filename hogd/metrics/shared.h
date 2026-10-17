#pragma once

#include <chrono>
#include <format>
#include <string>
#include <sys/resource.h>
#include <sys/time.h> // IWYU pragma: keep
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

// The process's total CPU time and how many CPUs it may run on, under the
// names Prometheus client libraries use, so that two scrapes alone say how
// many cores the process kept busy in between.
inline std::string
process_cpu_metrics()
{
  rusage usage{};
  ::getrusage(RUSAGE_SELF, &usage);
  // NOLINTBEGIN(misc-include-cleaner): glibc defines timeval internally
  auto seconds = [](const timeval &tv) {
    return static_cast<double>(tv.tv_sec) +
           (static_cast<double>(tv.tv_usec) / 1e6);
  };
  // NOLINTEND(misc-include-cleaner)
  return std::format(
      "# HELP process_cpu_seconds_total Total user and system CPU time spent "
      "in seconds.\n"
      "# TYPE process_cpu_seconds_total counter\n"
      "process_cpu_seconds_total {:.6f}\n"
      "# HELP process_available_cpus CPUs this process may run on.\n"
      "# TYPE process_available_cpus gauge\n"
      "process_available_cpus {}\n",
      seconds(usage.ru_utime) + seconds(usage.ru_stime),
      available_parallelism());
}

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
    return HttpResponse{
        .status = 200,
        .content_type = "text/plain",
        .body = Metrics::instance().dump_to_prometheus_string() +
                process_cpu_metrics(),
    };
  });
}

} // namespace hogd
