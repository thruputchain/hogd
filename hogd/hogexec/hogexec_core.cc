#include "hogd/hogexec/hogexec_core.h"

#include <string>

#include "hogd/coord/allocate_service.h"
#include "hogd/hogexec/interface/rpc_interface.h"

namespace hogd {

HogexecCore::HogexecCore(std::unique_ptr<PoolAndCoroPoll> pool_cp,
                         std::string const &coord_server_port,
                         ExecutorInitialization const &init_params)
  : pool_cp_(std::move(pool_cp)),
    db_proxy_(pool_cp_->cp_, init_params.database_info),
    exec_to_coord_server_(init_params.executor_id, db_proxy_)
{

  auto lfd = x23::tcplisten(atoi(coord_server_port.c_str()));

  std::fflush(stdout);
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  [](x23::CoroPoll &cp, ExecutorRpcInterface *exec_to_coord_server,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await cp.pool_.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c).server(exec_to_coord_server);
    }
  }(pool_cp_->cp_, &exec_to_coord_server_, std::move(lfd));
}

HogexecCore
start_hogexec(int nthreads, std::string const &exec_to_coord_server_port,
              std::string const &coord_address,
              std::string const &coord_allocate_port)
{
  auto pool_cp = std::make_unique<PoolAndCoroPoll>(nthreads);
  auto coord_client =
      connect_to_coord_allocate_service(pool_cp->cp_, coord_address,
                                        coord_allocate_port);
  while (!coord_client) {
    // TODO(geoff) retry timeout or some other strategy
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    coord_client =
        connect_to_coord_allocate_service(pool_cp->cp_, coord_address,
                                          coord_allocate_port);
  }

  while (true) {
    auto init_params = (*coord_client).request_new_executor()();
    if (init_params.status() == CoordinatorAllocateStatus::kRetry) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    if (init_params.status() == CoordinatorAllocateStatus::kCatastropic) {
      perror("catastropic failure");
      exit(EXIT_FAILURE);
    }

    assert(init_params.status() == CoordinatorAllocateStatus::kReady);

    return HogexecCore(std::move(pool_cp), exec_to_coord_server_port,
                       init_params.executor_init());
  }
}

} // namespace hogd
