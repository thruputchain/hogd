#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "hogd/auth/auth_server.h"
#include "hogd/base/rpc_server.h"
#include "hogd/hogd_dev/dev_event_aggregator.h"
#include "hogd/hogd_dev/fake_coordinator.h"
#include "hogd/hogd_dev/hogd_dev_tmp_dir.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_auth.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/hogexec/interface/executor.h"
#include "hogd/mempool/rpc_from_auth.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/mempool/rpc_from_db.h"
#include "hogd/mempool/rpc_from_event.h"
#include "hogd/mempool/rpc_from_exec.h"
#include "hogd/mempool/shared.h"

namespace hogd {

class HogdDevNode {
public:
  HogdDevNode(std::string bind_host, std::uint16_t requested_scheduler_port);
  ~HogdDevNode();

  HogdDevNode(HogdDevNode const &) = delete;
  HogdDevNode &operator=(HogdDevNode const &) = delete;
  HogdDevNode(HogdDevNode &&) = delete;
  HogdDevNode &operator=(HogdDevNode &&) = delete;

  std::string const &bind_host() const { return bind_host_; }
  // The supervisor calls this the scheduler endpoint; hogd exposes it through
  // the coordinator-facing SchedulerCoord1 RPC server.
  std::uint16_t scheduler_port() const { return mempool_coord_server_.port(); }

private:
  // Numeric chain network id. The readiness network_id is a human protocol
  // label consumed by the supervisor.
  static constexpr std::uint64_t kNetworkId = 0;
  // RAMDB maps this capacity but does not consume it as steady-state RSS.
  static constexpr std::size_t kRamDbBytes = 15UL * 1024 * 1024 * 1024;
  static constexpr std::size_t kDevRpcMaxMessageSize = 0x7fffffffff;

  void init_db_backend(std::unique_ptr<sched_db_client_t> sched_db_client);
  void register_db_with_mempool();
  void register_event_with_mempool();
  void register_auth_with_mempool();
  void register_executor_with_mempool();
  x23::Async<void> register_coordinator_with_mempool();

  std::string bind_host_;

  HogdDevTmpDir db_dir_;
  std::unique_ptr<RPCContext> ctx_;

  std::unique_ptr<HogDBExecutorServer> db_executor_server_;
  std::unique_ptr<HogDBSchedServer> db_scheduler_server_;
  std::unique_ptr<HogDBAuthServer> db_auth_server_;

  DevEventAggregatorSchedServer event_sched_server_;
  DevEventAggregatorExecServer event_exec_server_;

  Executor executor_;

  RpcServer<DevCoordinatorSchedRpc> coordinator_sched_server_;

  RpcServer<MempoolDbRpc> sched_db_server_;
  RpcServer<MempoolCoordinatorRpc> mempool_coord_server_ =
      RpcServer<MempoolCoordinatorRpc>(sched_db_server_->impl(), kNetworkId);
  RpcServer<MempoolAuthRpc> mempool_auth_server_;
  RpcServer<MempoolEventRpc> sched_event_server_;
  RpcServer<MempoolExecutorRpc> sched_exec_server_;

  std::unique_ptr<AuthServer> auth_server_;
  RpcServer<AuthDBRpc> auth_db_server_;
  RpcServer<AuthMempoolRpc> auth_mempool_server_;
};

} // namespace hogd
