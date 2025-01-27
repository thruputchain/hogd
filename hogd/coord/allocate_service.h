#pragma once

#include "coord.hh"
#include "hogd/coord/executor_manager.h"
#include "x23/event.h"

namespace hogd {

class CoordinatorAllocateService {
public:
  using rpc_service = CoordinatorAllocateV1;

  CoordinatorAllocateService(x23::CoroPoll &cp,
                             ExecutorManager &executor_manager)
    : cp_(cp), executor_manager_(executor_manager)
  {}

  // Disable copy and move
  CoordinatorAllocateService(CoordinatorAllocateService const &) = delete;
  CoordinatorAllocateService &
  operator=(CoordinatorAllocateService const &) = delete;
  CoordinatorAllocateService(CoordinatorAllocateService &&) = delete;
  CoordinatorAllocateService &operator=(CoordinatorAllocateService &&) = delete;

  x23::Async<ExpectedExecutorInitialization> request_new_executor();
  x23::Async<CoordinatorAllocateStatus>
  executor_announce_ready(ExecutorReadyAnnouncement const &announcement);

  x23::Async<CoordinatorAllocateStatus>
  database_announce_ready(DatabaseInfo const &info);

private:
  x23::CoroPoll &cp_;
  ExecutorManager &executor_manager_;
  std::mutex mtx_;
  uint16_t executor_id_ = 0;
  std::optional<DatabaseInfo> database_info_;
};

static_assert(
    is_CoordinatorAllocateV1<CoordinatorAllocateService *, x23::Async>);

} // namespace hogd
