#include "hogd/coord/allocate_service.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include "x23/event.h"

namespace hogd {

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
  auto fd = cp_.tcpconnect(connection.caller_address,
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
                   .database_port = announcement.database_port};
  co_return CoordinatorAllocateStatus::kReady;
}

namespace {
std::string
read_client_address(struct sockaddr const &client_addr)
{
  char buf[NI_MAXHOST];
  getnameinfo(&client_addr, sizeof(client_addr), buf, sizeof(buf), nullptr, 0,
              NI_NUMERICHOST);
  std::printf("client_address: %s\n", buf);
  return std::string(buf);
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
    struct sockaddr client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (auto c = co_await cp.accept(lfd, &client_addr, &addr_len)) {

      std::string client_address = read_client_address(client_addr);
      x23::rpc_stream_logged(cp, *c).server(
          service, CoordinatorAllocateV1{},
          CoordinatorAllocateServiceConnection{client_address});
    }
  }(cp_, this, std::move(lfd));
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
