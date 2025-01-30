#include "hogd/coord/allocate_service.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include "hogd/coord/database_manager.h"
#include "x23/event.h"

namespace hogd {

CoordinatorAllocateService::CoordinatorAllocateService(
    x23::CoroPoll &cp, ExecutorManager &executor_manager)
  : cp_(cp), executor_manager_(executor_manager)
{}

x23::Async<ExpectedExecutorInitialization>
CoordinatorAllocateService::request_new_executor(
    CoordinatorAllocateServiceConnection const & /*unused*/)
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
    ExecutorReadyAnnouncement const &announcement,
    CoordinatorAllocateServiceConnection const &connection)
{
  auto fd = co_await cp_.tcpconnect(connection.caller_address,
                                    std::string(announcement.executor_port));

  if (!fd) {
    co_return CoordinatorAllocateStatus::kCatastropic;
  }

  auto client_config = x23::rpc_stream_logged(cp_, *fd);
  auto executor = std::make_unique<ExecutorQueue>(
      announcement.nthreads,
      client_config.template client<ExecutorInterfaceV1>(), cp_);
  executor_manager_.add_executor(std::move(executor));
  co_return CoordinatorAllocateStatus::kReady;
}

x23::Async<CoordinatorAllocateStatus>
CoordinatorAllocateService::database_announce_ready(
    DatabaseReadyAnnouncement const &announcement,
    CoordinatorAllocateServiceConnection const &connection)
{
  std::lock_guard lk(mtx_);
  if (database_info_) {
    // TODO(geoff): status with multiple databases
    co_return CoordinatorAllocateStatus::kCatastropic;
  }
  database_info_ =
      DatabaseInfo{.database_address = {connection.caller_address.c_str(),
                                        connection.caller_address.size()},
                   .database_to_coord_port =
                       announcement.database_to_coord_port,
                   .database_to_exec_port = announcement.database_to_exec_port};
  cv_.notify_one();
  co_return CoordinatorAllocateStatus::kReady;
}

namespace {
std::string
read_client_address(struct sockaddr const &client_addr)
{
  const struct sockaddr *sa = &client_addr;
  char hbuf[NI_MAXHOST];

  if (getnameinfo(sa, sa->sa_len, hbuf, sizeof(hbuf), nullptr, 0,
                  NI_NUMERICHOST)) {
    perror("could not get numeric hostname");
    exit(EXIT_FAILURE);
  }
  return std::format("{}", hbuf);
}
} // namespace

void
CoordinatorAllocateService::start(std::string const &coord_server_port)
{
  auto lfd = x23::tcplisten(atoi(coord_server_port.c_str()));
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  [](x23::CoroPoll &cp, CoordinatorAllocateService *service,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await cp.pool_.bg();

    while (true) {
      struct sockaddr_storage client_addr;
      socklen_t addr_len = sizeof(client_addr);
      auto c = co_await cp.accept(lfd, &client_addr, &addr_len);
      if (!c) {
        break;
      }

      std::string client_address = read_client_address(
          reinterpret_cast<struct sockaddr const &>(client_addr));
      x23::rpc_stream_logged(cp, *c).server(
          service, CoordinatorAllocateV1{},
          CoordinatorAllocateServiceConnection{client_address});
    }
  }(cp_, this, std::move(lfd));
}

namespace {
std::optional<hogd::DatabaseToCoordInterfaceV1::client<
    x23::RpcStream<x23::RpcLogger>::ClientRef>>
connect_to_database(x23::CoroPoll &cp, DatabaseInfo const &database_info)
{
  auto fd = cp.tcpconnect(std::string(database_info.database_address),
                          std::string(database_info.database_to_coord_port))();
  if (!fd) {
    return std::nullopt;
  }

  auto client_config = x23::rpc_stream_logged(cp, *fd);
  return std::make_optional(
      client_config.template client<DatabaseToCoordInterfaceV1>());
}
} // namespace

std::unique_ptr<DatabaseManager>
CoordinatorAllocateService::make_database_manager()
{
  {
    std::unique_lock lk(mtx_);
    if (!database_info_) {
      cv_.wait(lk, [this] { return database_info_.has_value(); });
    }
  }

  std::unique_ptr<DatabaseManager> db_manager =
      std::make_unique<DatabaseManager>();

  auto db_client = connect_to_database(cp_, *database_info_);

  while (!db_client) {
    perror(
        std::format(
            "connect to database coord server {} port {} failed, retrying...",
            std::string(database_info_->database_address),
            std::string(database_info_->database_to_coord_port))
            .c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    db_client = connect_to_database(cp_, *database_info_);
  }

  db_manager->add_database_shard(std::move(*db_client));

  return db_manager;
}

std::optional<hogd::CoordinatorAllocateV1::client<
    x23::RpcStream<x23::RpcLogger>::ClientRef>>
connect_to_coord_allocate_service(x23::CoroPoll &cp,
                                  std::string const &coord_address,
                                  std::string const &coord_allocate_port)
{
  auto fd = cp.tcpconnect(coord_address, coord_allocate_port)();
  if (!fd) {
    return std::nullopt;
  }

  auto client_config = x23::rpc_stream_logged(cp, *fd);
  return std::make_optional(
      client_config.template client<CoordinatorAllocateV1>());
}

} // namespace hogd
