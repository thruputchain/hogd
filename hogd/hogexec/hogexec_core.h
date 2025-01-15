#pragma once

#include <string>

#include "hogd/hogexec/interface/rpc_interface.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

// Normal usage pattern:
// Step 1: Receive information from coordinator
// Step 2: Start hogexec core, start serving transaction requests
class HogexecCore {
public:
  HogexecCore(int nthreads, uint16_t executor_id, std::string coord_server_port,
              std::string db_address, std::string db_port);

  HogexecCore(HogexecCore const &) = delete;
  HogexecCore &operator=(HogexecCore const &) = delete;
  HogexecCore(HogexecCore &&) = delete;
  HogexecCore &operator=(HogexecCore &&) = delete;

private:
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;

  DatabaseProxy db_proxy_;

  ExecutorRpcInterface exec_to_coord_server_;
};

} // namespace hogd
