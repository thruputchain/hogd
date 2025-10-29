#include <cstdlib>
#include <format>
#include <future>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/socket.h>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/executor.h"
#include "hogd/init.h"
#include "port.hh"
#include "x23/sockio.h"

namespace hogd {

namespace {

int
hogexec_init(const std::string &coordinator_addr, int nthreads)
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

  Executor executor(nthreads);
  const sockaddr_storage coordinator_sockaddr =
      host_port_to_sockaddr(coordinator_host, coordinator_port);
  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  CHECK_EQ(executor.start(hostname, kExecutorMempoolServerPort,
                          coordinator_sockaddr),
           kExecutorMempoolServerPort);

  // block forever
  info("hogexec initialization complete.");
  std::promise<void>().get_future().wait();

  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

namespace {

void
usage(char *argv[])
{
  std::println("usage: {} coordinator_ip:port [-n <nthreads>]", argv[0]);
  exit(EXIT_FAILURE);
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting hogexec!");

  if (argc < 2) {
    usage(argv);
  }

  std::optional<int> nthreads;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc - 1, argv + 1, "n:")) != -1) {
    switch (opt) {
    case 'n':
      nthreads = std::stoi(optarg);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)

  if (!nthreads) {
    nthreads = hogd::available_parallelism();
  }

  const std::string coord_address = argv[1];
  return hogd::hogexec_init(coord_address, *nthreads);
}
