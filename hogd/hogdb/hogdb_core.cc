#include "hogd/hogdb/hogdb_core.h"

#include "hogd/hogdb/interface/block_manager.h"
#include "rpc.hh"
#include "x23/event.h"
#include "x23/fileio.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

// The genesis block BlockState is immediately ready for being
// GC'd from block manager, except that we need some block there as a root view.
HogdbCore::HogdbCore(int nthreads, std::string exec_server_port,
                     std::string coord_server_port)
  : pool_(nthreads), cp_(pool_),
    block_manager_(kGenesisBlockID, std::make_shared<BlockState>(&root_db_)),
    db_to_exec_server_(block_manager_), db_to_coord_server_(block_manager_)
{

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

  /*
    pool_.spawn([this, lfd{std::move(lfd)}] -> void {
      std::printf("start pool task\n");
      while (auto c = co_await cp_.accept(lfd)) {
        std::printf("got connect attempt\n");
        x23::rpc_stream_logged(cp_, *c).server(&db_to_exec_server_);
      }
      std::printf("shutdown\n");
      return;
    }); */

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

} // namespace hogd
