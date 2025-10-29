#include <cstdint>
#include <cstdlib>
#include <print>
#include <string>

#include "hogd/init.h"
#include "port.hh"

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} <default port name>", argv[0]);
  std::println("Default port numbers:");
  // database
  std::println("  kDbExecutorServerPort: {}", hogd::kDbExecutorServerPort);
  std::println("  kDbMempoolServerPort: {}", hogd::kDbMempoolServerPort);
  // mempool
  std::println("  kMempoolCoordinatorServerPort: {}",
               hogd::kMempoolCoordinatorServerPort);
  std::println("  kMempoolExecutorServerPort: {}",
               hogd::kMempoolExecutorServerPort);
  std::println("  kMempoolDbServerPort: {}", hogd::kMempoolDbServerPort);
  std::println("  kMempoolEventServerPort: {}", hogd::kMempoolEventServerPort);
  // executor
  std::println("  kExecutorMempoolServerPort: {}",
               hogd::kExecutorMempoolServerPort);
  // event
  std::println("  kEventExecutorServerPort: {}",
               hogd::kEventExecutorServerPort);
  std::println("  kEventMempoolServerPort: {}", hogd::kEventMempoolServerPort);
  // ci control
  std::println("  kCIControlServerPort: {}", hogd::kCIControlServerPort);
  exit(EXIT_FAILURE);
}

uint16_t
get_port(const std::string &port_name)
{
  // database
  if (port_name == "kDbExecutorServerPort") {
    return hogd::kDbExecutorServerPort;
  }

  if (port_name == "kDbMempoolServerPort") {
    return hogd::kDbMempoolServerPort;
  }

  // mempool
  if (port_name == "kMempoolCoordinatorServerPort") {
    return hogd::kMempoolCoordinatorServerPort;
  }

  if (port_name == "kMempoolExecutorServerPort") {
    return hogd::kMempoolExecutorServerPort;
  }

  if (port_name == "kMempoolDbServerPort") {
    return hogd::kMempoolDbServerPort;
  }

  if (port_name == "kMempoolEventServerPort") {
    return hogd::kMempoolEventServerPort;
  }
  // executor
  if (port_name == "kExecutorMempoolServerPort") {
    return hogd::kExecutorMempoolServerPort;
  }

  // event
  if (port_name == "kEventExecutorServerPort") {
    return hogd::kEventExecutorServerPort;
  }

  if (port_name == "kEventMempoolServerPort") {
    return hogd::kEventMempoolServerPort;
  }

  // ci control
  if (port_name == "kCIControlServerPort") {
    return hogd::kCIControlServerPort;
  }

  std::println("Invalid port name: {}", port_name);
  exit(EXIT_FAILURE);
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv, false);
  if (ret)
    return ret;

  if (argc < 2) {
    usage(argv);
  }

  std::println("{}", get_port(argv[1]));
  return EXIT_SUCCESS;
}
