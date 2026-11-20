#include <cstdint>
#include <cstdlib>
#include <print>
#include <string>

#include "ci_server.hh"
#include "hogd/init.h"

namespace {
void
usage(char *argv[])
{
  std::println("Usage: {} <default port name>", argv[0]);
  std::println("Default port numbers:");
  // CI control
  std::println("  kCIControlServerPort: {}", hogd::kCIControlServerPort);
  exit(EXIT_FAILURE);
}

uint16_t
get_port(const std::string &port_name)
{
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
