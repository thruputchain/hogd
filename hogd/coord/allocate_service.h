#pragma once

#include "coord.hh"
#include "hogd/coord/database_manager.h"
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

  // Waits until the database is ready, then returns a new DatabaseManager
  // connected to the database.
  std::unique_ptr<DatabaseManager> make_database_manager();

private:
  x23::CoroPoll &cp_;
  ExecutorManager &executor_manager_;
  std::mutex mtx_;
  std::condition_variable cv_; // for notify in make_database_manager
  uint16_t executor_id_ = 0;
  std::optional<DatabaseInfo> database_info_ = std::nullopt;
};

static_assert(is_CoordinatorAllocateV1<CoordinatorAllocateService *, x23::Async,
                                       CoordinatorAllocateServiceConnection>);

} // namespace hogd
