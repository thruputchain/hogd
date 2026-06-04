#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/health/health.h"
#include "hogd/httpd/httpd.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "x23/sockio.h"

namespace {

constexpr uint16_t kDefaultLoadgenPort = 0;

[[noreturn]] void
usage(const char *progname, int exit_code = EXIT_FAILURE)
{
  std::println(stderr,
               "Usage: {} [--port <port>] "
               "[--metrics <private-ip:port>] [--help]",
               progname);
  std::println(stderr, "Options:");
  std::println(stderr,
               "  --port <port>  Port number to listen on (default: {})",
               kDefaultLoadgenPort);
  std::println(stderr, "  --metrics <private-ip:port>  Health/metrics HTTP "
                       "endpoint address");
  std::println(stderr, "  --help         Show this help message");
  exit(exit_code);
}

std::optional<x23::HostPort>
parse_metrics_addr(std::string_view value)
{
  x23::HostPort host_port;
  CHECK(host_port.parse(value));
  CHECK(host_port.to_addr(/*dns=*/false).has_value());
  return host_port;
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  // Initialize hogd
  auto ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }

  const char *progname = argc > 0 ? argv[0] : "loadgen_server";
  uint16_t port = kDefaultLoadgenPort;
  std::optional<x23::HostPort> metrics_addr;

  // Parse command line arguments
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help") {
      usage(progname, EXIT_SUCCESS);
    }
    if (arg == "--port" && i + 1 < argc) {
      try {
        int parsed_port = std::stoi(argv[++i]);
        if (parsed_port < 0 || parsed_port > 65535) {
          std::println(stderr, "Error: Port must be between 0 and 65535");
          return EXIT_FAILURE;
        }
        port = static_cast<uint16_t>(parsed_port);
      } catch (const std::exception &) {
        std::println(stderr, "Error: Invalid port number '{}'", argv[i]);
        return EXIT_FAILURE;
      }
      continue;
    }
    if (arg == "--metrics" && i + 1 < argc) {
      metrics_addr = parse_metrics_addr(argv[++i]);
      continue;
    }
    std::println(stderr, "Error: Unexpected argument '{}'", arg);
    usage(progname);
  }

  hogd::info("Starting loadgen server on port {}", port);
  hogd::Health::instance().start_service();

  std::optional<hogd::httpd::HttpServer> metrics_server;
  if (metrics_addr.has_value()) {
    metrics_server.emplace();
    hogd::httpd::add_health_metrics_routes(*metrics_server);
    const uint16_t actual_metrics_port =
        metrics_server->start_server(std::move(*metrics_addr));
    hogd::info("PORT loadgen.metrics={}", actual_metrics_port);
  }

  // Create and start the loadgen server
  hogd::RpcServer<hogd::LoadgenRpc> server;
  // We set the [max_message_size_] to a large value because the loadgen
  // transfers a lot of data via its RPC calls.
  server->max_message_size_ = hogd::LoadgenRpc::kRpcMaxMessageSize;
  auto actual_port = server.start_server(port);
  hogd::Health::instance().set_status(hogd::Health::Status::kOk)();
  hogd::info("Loadgen server started successfully on port {}", actual_port);

  // block forever
  std::promise<void>().get_future().wait();

  return EXIT_SUCCESS;
}
