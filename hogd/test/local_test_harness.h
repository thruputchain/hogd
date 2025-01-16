#pragma once

#include "hogd/hogdb/hogdb_core.h"
#include "hogd/hogexec/hogexec_core.h"
#include "rpc.hh"
#include "x23/event.h"
#include "x23/threadpool.h"

namespace hogd ::test {

// Maintain a database and an executor
// within one binary.  Used only for testing.
struct LocalTestHarness {

  constexpr static std::string kDBToExecutorPort = "12000";
  constexpr static std::string kDBToCoordPort = "12001";
  constexpr static std::string kExecToCoordPort = "12002";

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

} // namespace hogd::test
