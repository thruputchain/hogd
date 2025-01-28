#pragma once

#include <string>

#include "coord.hh"
#include "hogd/hogexec/interface/rpc_interface.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd {

struct PoolAndCoroPoll {
  PoolAndCoroPoll(int nthreads) : pool_(nthreads), cp_(pool_) {}
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;
};

// Normal usage pattern:
// Step 1: Receive information from coordinator
// Step 2: Start hogexec core, start serving transaction requests
class HogexecCore {
public:
  HogexecCore(std::unique_ptr<PoolAndCoroPoll> pool_cp,
              std::string const &coord_server_port,
              ExecutorInitialization const &init_params);

  HogexecCore(HogexecCore const &) = delete;
  HogexecCore &operator=(HogexecCore const &) = delete;
  HogexecCore(HogexecCore &&) = delete;
  HogexecCore &operator=(HogexecCore &&) = delete;

private:
  std::unique_ptr<PoolAndCoroPoll> pool_cp_;

  DatabaseProxy db_proxy_;

  ExecutorRpcInterface exec_to_coord_server_;
};

HogexecCore start_hogexec(int nthreads,
                          std::string const &exec_to_coord_server_port,
                          std::string const &coord_address,
                          std::string const &coord_allocate_port);

} // namespace hogd
