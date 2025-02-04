#include "hogd/hogdb/hogdb_core.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "coord.hh"
#include "hogd/coord/allocate_service.h"
#include "hogd/hogdb/interface/block_manager.h"
#include "hogd/hogdb/interface/db_to_coord_server.h"
#include "hogd/hogdb/interface/db_to_executor_server.h"
#include "hogd/hogdb/softstate/block_state.h"
#include "rpc.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/fileio.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

// The genesis block BlockState is immediately ready for being
// GC'd from block manager, except that we need some block there as a root view.
HogdbCore::HogdbCore(int nthreads, std::string const &exec_server_port,
                     std::string const &coord_server_port)
  : pool_(nthreads), cp_(pool_),
    block_manager_(kGenesisBlockID, std::make_shared<BlockState>(&root_db_),
                   &root_db_),
    db_to_exec_server_(block_manager_), db_to_coord_server_(block_manager_),
    exec_server_port_(exec_server_port), coord_server_port_(coord_server_port)
{
  x23::RpcLogger::default_logfd = 2;

  auto lfd = x23::tcplisten(atoi(exec_server_port.c_str()));
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  // Cannot use captures in a lambda like this one -- the lambda
  // object is destroyed, probably, before the coroutine resumes.
  // Arguments to the lambda, instead, are captured on the coroutine stack.
  [](x23::CoroPoll &cp, x23::ThreadPool &pool,
     DBToExecutorServer *db_to_exec_server,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await pool.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c).server(db_to_exec_server);
    }
  }(cp_, pool_, &db_to_exec_server_, std::move(lfd));

  lfd = x23::tcplisten(atoi(coord_server_port.c_str()));
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  [](x23::CoroPoll &cp, x23::ThreadPool &pool,
     DBToCoordServer *db_to_coord_server, x23::UniqueFd lfd) -> x23::Detached {
    co_await pool.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c).server(db_to_coord_server);
    }
  }(cp_, pool_, &db_to_coord_server_, std::move(lfd));
}

void
HogdbCore::announce_to_coordinator(std::string const &coordinator_address,
                                   std::string const &coordinator_port)
{
  auto coord_client =
      connect_to_coord_allocate_service(cp_, coordinator_address,
                                        coordinator_port);
  while (!coord_client) {
    // TODO(geoff) retry timeout or some other strategy
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    coord_client = connect_to_coord_allocate_service(cp_, coordinator_address,
                                                     coordinator_port);
  }

  coord_client->database_announce_ready(DatabaseReadyAnnouncement{
      .database_to_coord_port = {coord_server_port_.c_str(),
                                 coord_server_port_.size()},
      .database_to_exec_port = {exec_server_port_.c_str(),
                                exec_server_port_.size()},
  })();
}

void
HogdbCore::await_shutdown()
{
  std::mutex mtx;
  std::unique_lock lock(mtx);
  std::condition_variable cv;
  cv.wait(lock, [] { return false; });
}

} // namespace hogd
