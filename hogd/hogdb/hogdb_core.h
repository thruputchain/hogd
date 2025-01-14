#pragma once

#include "hogd/hogdb/db/in_memory_database.h"
#include "hogd/hogdb/interface/block_manager.h"
#include "hogd/hogdb/interface/db_to_coord_server.h"
#include "hogd/hogdb/interface/db_to_executor_server.h"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

// Normal usage pattern:
// Step 1: Collect information from disk and from coordinator
// and from disk about database state
// Step 2: Create this object, which runs the executor- and coordinator-facing
// servers
class HogdbCore {
public:
  HogdbCore(int nthreads, std::string exec_server_port,
            std::string coord_server_port);

  HogdbCore(HogdbCore const &) = delete;
  HogdbCore &operator=(HogdbCore const &) = delete;
  HogdbCore(HogdbCore &&) = delete;
  HogdbCore &operator=(HogdbCore &&) = delete;

  void connect_to_coord(std::string hostname, std::string port);

private:
  const std::string progname_ = "hogdb";

  x23::ThreadPool pool_;
  x23::CoroPoll cp_;

  InMemoryDatabase root_db_;
  BlockManager block_manager_;

  DBToExecutorServer db_to_exec_server_;
  DBToCoordServer db_to_coord_server_;
};

} // namespace hogd
