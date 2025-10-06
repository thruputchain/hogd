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
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
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
constexpr uint16_t kEventServerPort = 8073;

int
mempool_init(size_t num_db_shards, size_t num_exec_shards)
{
  Registration &registration = Registration::registration();

  // Wait for the db shards to connect.
  RpcServer<MempoolDbRpc> db;
  CHECK_EQ(db.start_server(kDbServerPort), kDbServerPort);
  MempoolDbRpc &db_rpc = db.get_impl();
  info("Waiting for {} db shards to connect...", num_db_shards);
  while (registration.shards_registered(/*cards=*/nullptr).size() <
         num_db_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Wait for the event subsystem to connect.
  RpcServer<MempoolEventRpc> event;
  CHECK_EQ(event.start_server(kEventServerPort), kEventServerPort);
  MempoolEventRpc &event_rpc = event.get_impl();
  info("Waiting for event server to connect...");
  while (!registration.event_registered(/*card=*/nullptr)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Let the executors connect.
  RpcServer<MempoolExecutorRpc> exec(db_rpc, event_rpc);
  CHECK_EQ(exec.start_server(kExecutorServerPort), kExecutorServerPort);
  MempoolExecutorRpc &exec_rpc = exec.get_impl();
  info("Waiting for {} executors to connect...", num_exec_shards);
  while (registration.executors_registered(/*cards=*/nullptr) <
         num_exec_shards) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // Create the RPC clients that the mempool uses to communicate with the
  // coordinator, the executors, and the db shards.
  x23::ThreadPool client_pool;
  x23::CoroPoll cp(client_pool);
  coord_sched_client_t coord_client =
      MempoolCoordinatorRpc::get_client(coordinator_addr, cp);
  // TODO(jhumphri): I think I just need to create a client for one of the
  // shards? Will the account information that the scheduler needs be
  // distributed across shards?
  x23::Vec<db_sched_client_t> db_clients = db_rpc.get_clients(cp);
  x23::Vec<exec_sched_client_t> exec_clients = exec_rpc.get_clients(cp);
  auto event_sched_client = event_rpc.get_client(cp);

  // TODO(jhumphri): Register the mempool with the coordinator with
  // [coordscheduler_intro_sched()].
  RpcServer<MempoolCoordinatorRpc> coord(std::move(coord_client),
                                         std::move(db_clients[0]),
                                         std::move(exec_clients),
                                         std::move(event_sched_client));
  CHECK_EQ(coord.start_server(kCoordinatorServerPort), kCoordinatorServerPort);

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

  if (argc != 3) {
    std::println("usage: num_db_shards num_executor_shards");
    return EXIT_FAILURE;
  }

  size_t num_db_shards = std::stoul(argv[1], nullptr, 0);
  if (num_db_shards <= 0) {
    std::println("[num_db_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }
  size_t num_exec_shards = std::stoul(argv[1], nullptr, 0);
  if (num_exec_shards <= 0) {
    std::println("[num_exec_shards] must be greater than 0.");
    return EXIT_FAILURE;
  }
  return hogd::mempool_init(num_db_shards, num_exec_shards);
};
