#include <cstdlib>
#include <format>
#include <print>
#include <stdexcept>
#include <string>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/types.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

constexpr uint16_t kCoordinatorServerPort = 8070;
constexpr uint16_t kExecutorServerPort = 8071;
constexpr uint16_t kDbServerPort = 8072;

std::string
create_address(uint16_t port)
{
  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  return std::format("{}:{}", hostname, port);
}

int
mempool_init(const std::string &coordinator_addr)
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

  MempoolExecutorServer exec;
  // exec.start_server() ...

  MempoolDbServer db;
  // db.start_server() ...

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
  hogd::info("starting mempool!");

  if (argc != 2) {
    std::println("usage: coordinator_ip:port");
    return EXIT_FAILURE;
  }

  const std::string coord_address = argv[1];
  return hogd::mempool_init(coord_address);
};
