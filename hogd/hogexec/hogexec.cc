#include <cstdlib>
#include <print>
#include <string>

#include "coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/exec_coord_server.h"
#include "hogd/init.h"

namespace hogd {

namespace {

constexpr uint16_t kExecServerPort = 8070;
const std::string kExecutorAddress =
    "127.0.0.1:" + std::to_string(hogd::kExecServerPort);
const ExecCard kCard{.protocol = "blah",
                     .upid = {},
                     .address = kExecutorAddress.c_str()};

int
hogexec_init(const std::string &coordinator_addr, int poll_threads)
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

  ExecCoordServer exec(kCard, coordinator_host, coordinator_port, poll_threads);
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

  if (argc < 3) {
    std::println("usage: coordinator_ip:port num_poll_threads");
    return EXIT_FAILURE;
  }

  const std::string kCoordAddress = argv[1];
  const int poll_threads = std::stoi(argv[2]);
  return hogd::hogexec_init(kCoordAddress, poll_threads);
};
