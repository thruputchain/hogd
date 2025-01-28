#include "hogd/test/local_test_harness.h"

namespace hogd ::test {

LocalTestHarness::LocalTestHarness()
  : db_core_(1, kDBToExecutorPort, kDBToCoordPort),
    exec_core_(
        std::make_unique<PoolAndCoroPoll>(1), kExecToCoordPort,
        ExecutorInitialization{
            .database_info =
                DatabaseInfo{.database_address = "localhost",
                             .database_port = {kDBToExecutorPort.c_str(),
                                               kDBToExecutorPort.size()}}}),
    pool_(1), cp_(pool_), db_client_([this] {
      x23::RpcLogger::default_logfd = 2;

      auto fd = cp_.tcpconnect("localhost", kDBToCoordPort)();
      if (!fd) {
        exit(-1);
      }

      auto client_config = x23::rpc_stream_logged(cp_, *fd);
      return client_config.template client<DatabaseToCoordInterfaceV1>();
    }()),
    exec_client_([this] {
      auto fd = cp_.tcpconnect("localhost", kExecToCoordPort)();
      if (!fd) {
        exit(-1);
      }

      auto client_config = x23::rpc_stream_logged(cp_, *fd);
      return client_config.template client<ExecutorInterfaceV1>();
    }())
{}

} // namespace hogd::test
