#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <print>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/health/health.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"

namespace {

constexpr uint16_t kDefaultLoadgenPort = 0;

[[noreturn]] void
usage(const char *progname, int exit_code = EXIT_FAILURE)
{
  std::println(stderr, "Usage: {} [--port <port>] [--help]", progname);
  std::println(stderr, "Options:");
  std::println(stderr,
               "  --port <port>  Port number to listen on (default: {})",
               kDefaultLoadgenPort);
  std::println(stderr, "  --help         Show this help message");
  exit(exit_code);
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
    std::println(stderr, "Error: Unexpected argument '{}'", arg);
    usage(progname);
  }

  hogd::info("Starting loadgen server on port {}", port);
  hogd::Health::instance().start_service();

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
