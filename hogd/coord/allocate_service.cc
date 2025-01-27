#include "hogd/coord/allocate_service.h"

#include "x23/event.h"

namespace hogd {

x23::Async<ExpectedExecutorInitialization>
CoordinatorAllocateService::request_new_executor()
{
  std::lock_guard lk(mtx_);
  if (!database_info_) {
    co_return ExpectedExecutorInitialization(CoordinatorAllocateStatus::kRetry);
  }
  uint32_t executor_id = executor_id_++;
  assert(executor_id <= std::numeric_limits<uint16_t>::max());

  ExpectedExecutorInitialization init(CoordinatorAllocateStatus::kReady);
  init.executor_init() =
      ExecutorInitialization{.executor_id = executor_id,
                             .database_info = *database_info_};
  co_return init;
}

x23::Async<CoordinatorAllocateStatus>
CoordinatorAllocateService::executor_announce_ready(
    ExecutorReadyAnnouncement const &announcement)
{
  auto fd = cp_.tcpconnect(std::string(announcement.executor_address),
                           std::string(announcement.executor_port))();
  if (!fd) {
    co_return CoordinatorAllocateStatus::kCatastropic;
  }

  auto client_config = x23::rpc_stream_logged(cp_, *fd);
  auto executor = std::make_unique<ExecutorQueue>(
      announcement.nthreads,
      client_config.template client<ExecutorInterfaceV1>());
  executor_manager_.add_executor(std::move(executor));
  co_return CoordinatorAllocateStatus::kReady;
}

x23::Async<CoordinatorAllocateStatus>
CoordinatorAllocateService::database_announce_ready(DatabaseInfo const &info)
{
  std::lock_guard lk(mtx_);
  if (database_info_) {
    // TODO(geoff): status with multiple databases
    co_return CoordinatorAllocateStatus::kCatastropic;
  }
  database_info_ = info;
  co_return CoordinatorAllocateStatus::kReady;
}

} // namespace hogd
