#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_server.h"
#include "hogd/http/http.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/metrics/shared.h"

namespace {

constexpr uint16_t kDefaultLoadgenPort = 0;

[[noreturn]] void
usage(const char *progname, int exit_code = EXIT_FAILURE)
{
  std::println(stderr,
               "Usage: {} --bind-ip <ip> [--port <port>] "
               "[--metrics-port <port>] [--help]",
               progname);
  std::println(stderr, "Options:");
  std::println(stderr, "  --bind-ip <ip>  IP address to bind metrics on");
  std::println(stderr,
               "  --port <port>  Port number to listen on (default: {})",
               kDefaultLoadgenPort);
  std::println(stderr, "  --metrics-port <port>  Health/metrics HTTP port "
                       "(default: 0)");
  std::println(stderr, "  --help         Show this help message");
  exit(exit_code);
}

uint16_t
parse_port(const std::string &value)
{
  size_t pos = 0;
  uint64_t port = std::stoul(value, &pos, 10);
  if (pos != value.size() || port > 65535) {
    hogd::panic("Invalid port: {}", value);
  }
  return static_cast<uint16_t>(port);
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
  std::string bind_ip;
  uint16_t port = kDefaultLoadgenPort;
  uint16_t metrics_port = 0;

  // Parse command line arguments
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help") {
      usage(progname, EXIT_SUCCESS);
    }
    if (arg == "--bind-ip" && i + 1 < argc) {
      bind_ip = argv[++i];
      continue;
    }
    if (arg == "--port" && i + 1 < argc) {
      port = parse_port(argv[++i]);
      continue;
    }
    if (arg == "--metrics-port" && i + 1 < argc) {
      metrics_port = parse_port(argv[++i]);
      continue;
    }
    std::println(stderr, "Error: Unexpected argument '{}'", arg);
    usage(progname);
  }

  if (bind_ip.empty()) {
    usage(progname);
  }

  hogd::info("Starting loadgen server on port {}", port);
  hogd::HttpServer metrics_server;
  hogd::add_health_metrics_routes(metrics_server);
  const uint16_t actual_metrics_port = metrics_server.start_server(
      hogd::make_bind_host_port(bind_ip, metrics_port));
  hogd::info("PORT loadgen.metrics={}", actual_metrics_port);

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
