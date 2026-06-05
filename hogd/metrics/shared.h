#pragma once

#include <chrono>
#include <format>
#include <string>
#include <thread>

#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/http/http.h"
#include "hogd/metrics/health.h"
#include "hogd/metrics/new_metrics.h"
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/progname.h"

namespace hogd {

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
  server.add_route("/metrics", []() -> x23::Async<HttpResponse> {
    co_return HttpResponse{
        .status = 200,
        .content_type = "text/plain",
        .body = co_await Metrics::instance().dump_to_prometheus_string(),
    };
  });
}

} // namespace hogd
