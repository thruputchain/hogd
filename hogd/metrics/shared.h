#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/time.h> // IWYU pragma: keep
#include <system_error>
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

// One line per thread of this process: "<tid> <name> <user+system ticks>",
// from /proc/self/task, for load generators that want to know which thread
// is busy.  Threads that exit while it is being read are skipped.
inline std::string
thread_cpu_ticks()
{
  std::ostringstream out;
  std::error_code ec;
  for (const auto &entry :
       std::filesystem::directory_iterator("/proc/self/task", ec)) {
    std::ifstream in(entry.path() / "stat");
    std::string stat((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    // "tid (name) state ..." with utime and stime the 12th and 13th fields
    // after the closing parenthesis; the name itself may hold spaces.
    const size_t open = stat.find('(');
    const size_t close = stat.rfind(')');
    if (open == std::string::npos || close == std::string::npos) {
      continue;
    }
    std::istringstream fields(stat.substr(close + 1));
    std::string field;
    uint64_t ticks = 0;
    for (int i = 0; i < 13 && fields >> field; i++) {
      if (i >= 11) {
        ticks += std::strtoull(field.c_str(), nullptr, 10);
      }
    }
    out << entry.path().filename().string() << ' '
        << stat.substr(open + 1, close - open - 1) << ' ' << ticks << '\n';
  }
  return out.str();
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
  // The host's /proc/stat as is, for load generators: per-core busy and
  // softirq time cover kernel work (network receive processing above all)
  // that a process's own CPU time does not.
  server.add_route("/threadstat", [] {
    return HttpResponse{
        .status = 200,
        .content_type = "text/plain",
        .body = thread_cpu_ticks(),
    };
  });
  server.add_route("/procstat", [] {
    std::ifstream in("/proc/stat");
    std::ostringstream body;
    body << in.rdbuf();
    return HttpResponse{
        .status = in ? 200 : 500,
        .content_type = "text/plain",
        .body = body.str(),
    };
  });
}

} // namespace hogd
