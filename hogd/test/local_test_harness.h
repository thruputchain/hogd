#pragma once

#include "hogd/hogdb/hogdb_core.h"
#include "hogd/hogexec/hogexec_core.h"
#include "rpc.hh"
#include "rpc_types.hh"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd ::test {

// Maintain a database and an executor
// within one binary.  Used only for testing.
struct LocalTestHarness {

  inline static std::string kDBToExecutorPort =
      std::to_string(kDBToExecutorPortNum);
  inline static std::string kDBToCoordPort = std::to_string(kDBToCoordPortNum);
  inline static std::string kExecToCoordPort =
      std::to_string(kExecToCoordPortNum);

  LocalTestHarness();

  HogdbCore db_core_;
  HogexecCore exec_core_;

  // for sending RPCS to db and exec
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;

  using db_client_t = DatabaseToCoordInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;
  using exec_client_t =
      ExecutorInterfaceV1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  db_client_t db_client_;
  exec_client_t exec_client_;
};

struct DatabaseWrapper {

protected:
  using coord_t = DatabaseToCoordInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;
  using exec_t = DatabaseToExecutorInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  const std::string kDBToExecutorPort = std::to_string(kDBToExecutorPortNum);
  const std::string kDBToCoordPort = std::to_string(kDBToCoordPortNum);

  exec_t connect_from_exec()
  {
    auto fd = cp_->tcpconnect(std::string("localhost"),
                              std::string(kDBToExecutorPort))();
    assert(fd);
    return x23::rpc_stream_logged(*cp_, *fd)
        .template client<DatabaseToExecutorInterfaceV1>();
  }

  coord_t connect_from_coord()
  {
    auto fd = cp_->tcpconnect(std::string("localhost"),
                              std::string(kDBToCoordPort))();
    assert(fd);
    return x23::rpc_stream_logged(*cp_, *fd)
        .template client<DatabaseToCoordInterfaceV1>();
  }

  void initialize()
  {
    pool_ = std::make_unique<x23::ThreadPool>(1);
    cp_ = std::make_unique<x23::CoroPoll>(*pool_);

    db_core_ =
        std::make_unique<HogdbCore>(1, kDBToExecutorPort, kDBToCoordPort);

    coord_client_ = std::make_unique<coord_t>(connect_from_coord());
    exec_client_ = std::make_unique<exec_t>(connect_from_exec());
  }

  std::unique_ptr<x23::ThreadPool> pool_;
  std::unique_ptr<x23::CoroPoll> cp_;

  std::unique_ptr<HogdbCore> db_core_;

  std::unique_ptr<coord_t> coord_client_;
  std::unique_ptr<exec_t> exec_client_;
};

} // namespace hogd::test
