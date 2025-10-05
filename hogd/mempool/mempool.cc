#include <chrono>
#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "coord_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/mempool/types.h"
#include "hogd/version.h"
#include "x23/event.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

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
mempool_init(const std::string &coordinator_addr, size_t num_db_shards,
             size_t num_exec_shards)
{
  x23::HostPort host_port;
  if (!host_port.parse(coordinator_addr)) {
    throw std::runtime_error(
        std::format("Invalid coordinator address: {}", coordinator_addr));
  }
  const std::string coordinator_host = {host_port.host.begin(),
                                        host_port.host.end()};
  // const uint16_t coordinator_port = static_cast<uint16_t>(
  //     std::stoul(std::string{host_port.port.begin(), host_port.port.end()}));

  // Wait for the db shards to connect.
  RpcServer<MempoolDbRpc> db;
  CHECK_EQ(db.start_server(kDbServerPort), kDbServerPort);
  MempoolDbRpc &db_rpc = db.get_impl();
  info("Waiting for {} db shards to connect...", num_db_shards);
  while (db_rpc.shards().size() < num_db_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Let the executors connect.
  RpcServer<MempoolExecutorRpc> exec(db_rpc);
  CHECK_EQ(exec.start_server(kExecutorServerPort), kExecutorServerPort);
  MempoolExecutorRpc &exec_rpc = exec.get_impl();
  info("Waiting for {} executors to connect...", num_exec_shards);
  while (exec_rpc.executors().size() < num_exec_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Create the RPC clients that the mempool uses to communicate with the
  // coordinator, the executors, and the db shards.
  x23::ThreadPool client_pool;
  x23::CoroPoll cp(client_pool);
  coord_client_t coord_client =
      MempoolCoordinatorRpc::get_client(coordinator_addr, cp);
  // TODO(jhumphri): I think I just need to create a client for one of the
  // shards? Will the account information that the scheduler needs be
  // distributed across shards?
  x23::Vec<db_sched_client_t> db_clients = db_rpc.get_clients(cp);
  x23::Vec<exec_client_t> exec_clients = exec_rpc.get_clients(cp);

  // TODO(jhumphri): Register the mempool with the coordinator with
  // [coordscheduler_intro_sched()].
  RpcServer<MempoolCoordinatorRpc> coord(std::move(coord_client),
                                         std::move(db_clients[0]),
                                         std::move(exec_clients));
  CHECK_EQ(coord.start_server(kCoordinatorServerPort), kCoordinatorServerPort);
  const std::string sched_address = create_address(kCoordinatorServerPort);
  SchedulerCard card{.protocol = version::kHogdProtocol,
                     .address = sched_address.c_str()};
  coord.get_impl().start(card);

  // The mempool is initialized. Now block forever.
  info("mempool initialization complete.");
  std::promise<void>().get_future().wait();

  // TODO(jhumphri): Implement shutdown path.
  // coord.shut_down();
  // exec.shut_down();
  // db.shut_down();

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

  if (argc != 4) {
    std::println(
        "usage: coordinator_ip:port num_db_shards num_executor_shards");
    return EXIT_FAILURE;
  }

  const std::string coord_address = argv[1];
  size_t num_db_shards = std::stoul(argv[2], nullptr, 0);
  if (num_db_shards <= 0) {
    std::println("[num_db_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }
  size_t num_exec_shards = std::stoul(argv[3], nullptr, 0);
  if (num_exec_shards <= 0) {
    std::println("[num_exec_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }
  return hogd::mempool_init(coord_address, num_db_shards, num_exec_shards);
};
