#include "hogd/test/local_test_harness.h"

#include <cstdlib>
#include <memory>

#include "coord.hh"
#include "hogd/hogexec/hogexec_core.h"
#include "rpc.hh"
#include "x23/rpc.h"

namespace hogd ::test {

LocalTestHarness::LocalTestHarness()
  : db_core_(1, db_to_executor_port_, db_to_coord_port_),
    exec_core_(
        std::make_unique<PoolAndCoroPoll>(1), exec_to_coord_port_,
        ExecutorInitialization{
            .database_info =
                DatabaseInfo{
                    .database_address = "localhost",
                    .database_to_coord_port = {db_to_coord_port_.c_str(),
                                               db_to_coord_port_.size()},
                    .database_to_exec_port = {db_to_executor_port_.c_str(),
                                              db_to_executor_port_.size()}}}),
    pool_(1), cp_(pool_), db_client_([this] {
      x23::RpcLogger::default_logfd = 2;

      auto fd = cp_.tcpconnect("localhost", db_to_coord_port_)();
      if (!fd) {
        exit(-1);
      }

      auto client_config = x23::rpc_stream_logged(cp_, *fd);
      return client_config.template client<DatabaseToCoordInterfaceV1>();
    }()),
    exec_client_([this] {
      auto fd = cp_.tcpconnect("localhost", exec_to_coord_port_)();
      if (!fd) {
        exit(-1);
      }

      auto client_config = x23::rpc_stream_logged(cp_, *fd);
      return client_config.template client<ExecutorInterfaceV1>();
    }())
{}

} // namespace hogd::test
