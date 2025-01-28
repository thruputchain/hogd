#pragma once

#include "coord.hh"
#include "hogd/coord/executor_manager.h"
#include "x23/event.h"

namespace hogd {

struct CoordinatorAllocateServiceConnection {
  std::string caller_address;
};

class CoordinatorAllocateService {
public:
  using rpc_service = CoordinatorAllocateV1;

  CoordinatorAllocateService(x23::CoroPoll &cp,
                             ExecutorManager &executor_manager);

  // Disable copy and move
  CoordinatorAllocateService(CoordinatorAllocateService const &) = delete;
  CoordinatorAllocateService &
  operator=(CoordinatorAllocateService const &) = delete;
  CoordinatorAllocateService(CoordinatorAllocateService &&) = delete;
  CoordinatorAllocateService &operator=(CoordinatorAllocateService &&) = delete;

  x23::Async<ExpectedExecutorInitialization>
  request_new_executor(CoordinatorAllocateServiceConnection const & /*unused*/);
  x23::Async<CoordinatorAllocateStatus> executor_announce_ready(
      ExecutorReadyAnnouncement const &announcement,
      CoordinatorAllocateServiceConnection const &connection);
  x23::Async<CoordinatorAllocateStatus> database_announce_ready(
      DatabaseReadyAnnouncement const &announcement,
      CoordinatorAllocateServiceConnection const &connection);

  void start(std::string const &coord_server_port);

private:
  x23::CoroPoll &cp_;
  ExecutorManager &executor_manager_;
  std::mutex mtx_;
  uint16_t executor_id_ = 0;
  std::optional<DatabaseInfo> database_info_;
};

static_assert(is_CoordinatorAllocateV1<CoordinatorAllocateService *, x23::Async,
                                       CoordinatorAllocateServiceConnection>);

std::optional<hogd::CoordinatorAllocateV1::client<
    x23::RpcStream<x23::RpcLogger>::ClientRef>>
connect_to_coord_allocate_service(x23::CoroPoll &cp,
                                  std::string const &coord_address,
                                  std::string const &coord_allocate_port);

} // namespace hogd
