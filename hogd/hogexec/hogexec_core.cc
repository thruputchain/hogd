#include "hogd/hogexec/hogexec_core.h"

#include <string>

#include "hogd/hogexec/interface/rpc_interface.h"
#include "rpc.hh"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

HogexecCore::HogexecCore(int nthreads, uint16_t executor_id,
                         std::string coord_server_port, std::string db_address,
                         std::string db_port)
  : pool_(nthreads), cp_(pool_), db_proxy_(cp_, db_address, db_port),
    exec_to_coord_server_(executor_id, db_proxy_)
{
  auto lfd = x23::tcplisten(atoi(coord_server_port.c_str()));
  if (!lfd) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  [](x23::CoroPoll &cp, ExecutorRpcInterface *exec_to_coord_server,
     x23::UniqueFd lfd) -> x23::Detached {
    co_await cp.pool_.bg();
    while (auto c = co_await cp.accept(lfd)) {
      x23::rpc_stream_logged(cp, *c).server(exec_to_coord_server);
      std::printf("accepted connection in executor\n");
    }
  }(cp_, &exec_to_coord_server_, std::move(lfd));
}

} // namespace hogd
