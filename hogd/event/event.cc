#include <cassert>
#include <chrono>
#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/types.h"
#include "hogd/event/rpc_context.h"
#include "hogd/event/rpc_from_exec.h"
#include "hogd/event/rpc_from_sched.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "rpc_types.hh"
#include "sched_event.hh"
#include "x23/cleanup.h"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

namespace {

constexpr uint16_t kExecutorServerPort = 8090;
constexpr uint16_t kSchedulerServerPort = 8091;

std::string
create_address(uint16_t port)
{
  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  return std::format("{}:{}", hostname, port);
}

auto
connect_to_event_sched_server(std::string const &address)
{
  x23::HostPort host_port;
  if (!host_port.parse(address)) {
    throw std::runtime_error("Failed to parse db server address");
  }
  auto server_addr = host_port.to_addr(true);
  if (!server_addr) {
    throw std::runtime_error("Failed to connect to db server");
  }

  while (true) {
    x23::UniqueFd fd;
    try {
      fd = x23::tcpconnect(server_addr.value());
    } catch (const std::system_error &e) {
      if (e.code() == std::errc::connection_refused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        warn_every_n_sec(5.0F, "Failed to connect to db server: {}", address);
        continue;
      }
      throw;
    }
    assert(fd);
    return x23::srpc_stream_logged(fd.release())->client(SchedEvent1{});
  }
}

int
event_init(const std::string &mempool_address,
           const std::string &work_directory)
{
  // Start the event servers.
  EventRPCContext ctx(work_directory);
  EventExecServer exec_server(ctx);
  EventSchedServer sched_server(ctx);

  CHECK_EQ(exec_server.start_server(kExecutorServerPort), kExecutorServerPort);
  CHECK_EQ(sched_server.start_server(kSchedulerServerPort),
           kSchedulerServerPort);

  // Register with the mempool.
  x23::HostPort host_port;
  if (!host_port.parse(mempool_address)) {
    throw std::runtime_error(
        std::format("Invalid mempool address: {}", mempool_address));
  }
  auto client = connect_to_event_sched_server(mempool_address);
  std::string address_for_exec = create_address(kExecutorServerPort);
  std::string address_for_sched = create_address(kSchedulerServerPort);
  EventCard card{.protocol = version::kHogdProtocol,
                 .upid = {},
                 .address_for_exec = address_for_exec.c_str(),
                 .address_for_sched = address_for_sched.c_str()};
  CHECK_EQ(client.schedevent_intro_event(card), RPCError::kRPCNone);

  // The event server is initialized. Now block forever.
  info("event initialization complete.");
  std::promise<void>().get_future().wait();

  // TODO(jhumphri): Implement shutdown path.

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

  if (argc != 3) {
    std::println("usage: <mempool_server_address> <work_directory>");
    return EXIT_FAILURE;
  }

  hogd::info("starting event server!");

  std::string mempool_address = argv[1];
  std::string work_directory = argv[2];
  return hogd::event_init(mempool_address, work_directory);
}
