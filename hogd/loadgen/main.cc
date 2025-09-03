#include <cstdint>
#include <cstdlib>
#include <exception>
#include <getopt.h>
#include <print>
#include <string>
#include <unistd.h>

#include "hogd/base/log.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"

namespace {

constexpr uint16_t kDefaultLoadgenPort = 8080;

[[noreturn]] void
usage(const char *progname, int exit_code = EXIT_FAILURE)
{
  std::println(stderr, "Usage: {} [-p <port>] [-h]", progname);
  std::println(stderr, "Options:");
  std::println(stderr, "  -p <port>  Port number to listen on (default: {})",
               kDefaultLoadgenPort);
  std::println(stderr, "  -h         Show this help message");
  exit(exit_code);
}

} // namespace

int
main(int argc, char *argv[]) noexcept
{
  try {
    // Initialize hogd
    int ret = hogd::hogd_init(argc, argv);
    if (ret) {
      return ret;
    }

    const char *progname = argc > 0 ? argv[0] : "loadgen_server";
    uint16_t port = kDefaultLoadgenPort;

    // Parse command line arguments
    int opt;
    while ((opt = getopt(argc, argv, "p:h")) != -1) {
      switch (opt) {
      case 'p':
        try {
          int parsed_port = std::stoi(optarg);
          if (parsed_port <= 0 || parsed_port > 65535) {
            std::println(stderr, "Error: Port must be between 1 and 65535");
            return EXIT_FAILURE;
          }
          port = static_cast<uint16_t>(parsed_port);
        } catch (const std::exception &e) {
          std::println(stderr, "Error: Invalid port number '{}'", optarg);
          return EXIT_FAILURE;
        }
        break;
      case 'h':
        usage(progname, EXIT_SUCCESS);
        break;
      default:
        usage(progname);
        break;
      }
    }

    // Check for extra arguments
    if (optind != argc) {
      std::println(stderr, "Error: Unexpected extra arguments");
      usage(progname);
    }

    hogd::info("Starting loadgen server on port {}", port);

    // Create and start the loadgen server
    hogd::LoadgenServer server;
    auto start_result = server.start_server(port);
    if (!start_result) {
      std::println(stderr, "Error: Failed to start server on port {}: {}", port,
                   start_result.error());
      return EXIT_FAILURE;
    }

    uint16_t actual_port = start_result.value();
    hogd::info("Loadgen server started successfully on port {}", actual_port);
    std::println("Loadgen server listening on port {}", actual_port);
    std::println("Press Enter to stop the server...");

    // Wait for user input to stop the server
    char buf;
    if (read(STDIN_FILENO, &buf, 1) > 0) {
      hogd::info("Stopping loadgen server");
      std::println("Server stopped");
    }

    return EXIT_SUCCESS;
  } catch (...) {
    std::println(stderr, "Error: Unexpected exception occurred");
    return EXIT_FAILURE;
  }
}
