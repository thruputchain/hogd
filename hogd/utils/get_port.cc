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
  std::println("  kDbAuthServerPort: {}", hogd::kDbAuthServerPort);
  // mempool
  std::println("  kMempoolCoordinatorServerPort: {}",
               hogd::kMempoolCoordinatorServerPort);
  std::println("  kMempoolExecutorServerPort: {}",
               hogd::kMempoolExecutorServerPort);
  std::println("  kMempoolDbServerPort: {}", hogd::kMempoolDbServerPort);
  std::println("  kMempoolEventServerPort: {}", hogd::kMempoolEventServerPort);
  std::println("  kMempoolAuthServerPort: {}", hogd::kMempoolAuthServerPort);

  // executor
  std::println("  kExecutorMempoolServerPort: {}",
               hogd::kExecutorMempoolServerPort);
  // event
  std::println("  kEventExecutorServerPort: {}",
               hogd::kEventExecutorServerPort);
  std::println("  kEventMempoolServerPort: {}", hogd::kEventMempoolServerPort);
  std::println("  kEventIndexerServerPort: {}", hogd::kEventIndexerServerPort);
  // auth
  std::println("  kAuthDbServerPort: {}", hogd::kAuthDbServerPort);
  std::println("  kAuthMempoolServerPort: {}", hogd::kAuthMempoolServerPort);
  // indexer
  std::println("  kIndexerBlockExplorerServerPort: {}", hogd::kIndexerBlockExplorerServerPort);
  std::println("  kIndexerEventServerPort: {}", hogd::kIndexerEventServerPort);
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

  if (port_name == "kDbAuthServerPort") {
    return hogd::kDbAuthServerPort;
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

  if (port_name == "kMempoolAuthServerPort") {
    return hogd::kMempoolAuthServerPort;
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

  if (port_name == "kEventIndexerServerPort") {
    return hogd::kEventIndexerServerPort;
  }

  // auth
  if (port_name == "kAuthDbServerPort") {
    return hogd::kAuthDbServerPort;
  }

  if (port_name == "kAuthMempoolServerPort") {
    return hogd::kAuthMempoolServerPort;
  }

  // indexer
  if (port_name == "kIndexerBlockExplorerServerPort") {
    return hogd::kIndexerBlockExplorerServerPort;
  }

  if (port_name == "kIndexerEventServerPort") {
    return hogd::kIndexerEventServerPort;
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
