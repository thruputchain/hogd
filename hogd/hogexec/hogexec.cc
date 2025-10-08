#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <stdexcept>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/executor.h"
#include "hogd/init.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

constexpr uint16_t kExecServerPort = 8070;

int
hogexec_init(const std::string &coordinator_addr)
{
  x23::HostPort host_port;
  if (!host_port.parse(coordinator_addr)) {
    throw std::runtime_error(
        std::format("Invalid coordinator address: {}", coordinator_addr));
  }
  const std::string coordinator_host = {host_port.host.begin(),
                                        host_port.host.end()};
  const uint16_t coordinator_port = static_cast<uint16_t>(
      std::stoul(std::string{host_port.port.begin(), host_port.port.end()}));

  Executor executor;
  const sockaddr_storage coordinator_sockaddr =
      host_port_to_sockaddr(coordinator_host, coordinator_port);
  CHECK_EQ(executor.start(kExecServerPort, coordinator_sockaddr),
           kExecServerPort);

  // block forever
  info("hogexec initialization complete.");
  std::promise<void>().get_future().wait();

  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting hogexec!");

  if (argc != 2) {
    std::println("usage: coordinator_ip:port");
    return EXIT_FAILURE;
  }

  const std::string coord_address = argv[1];
  return hogd::hogexec_init(coord_address);
};
