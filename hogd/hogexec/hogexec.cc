#include <cstdlib>
#include <format>
#include <print>
#include <stdexcept>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/exec_coord_server.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "sched_exec.hh"
#include "x23/sockio.h"

namespace hogd {

namespace {

constexpr uint16_t kExecServerPort = 8070;

std::string
get_executor_address()
{
  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  return std::format("{}:{}", hostname, kExecServerPort);
}

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

  const std::string executor_address = get_executor_address();
  const ExecCard card{.protocol = version::kHogdProtocol,
                      .upid = {},
                      .address = executor_address.c_str()};
  ExecCoordServer exec(card, coordinator_host, coordinator_port);
  // TODO(jhumphri): Replace nullptr.
  exec.init_from_coord(nullptr);
  CHECK_EQ(exec.start_server(kExecServerPort), kExecServerPort);

  return 0;
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
