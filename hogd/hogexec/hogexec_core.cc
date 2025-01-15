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

  pool_.spawn([this, lfd{std::move(lfd)}] -> x23::Detached {
    while (auto c = co_await cp_.accept(lfd)) {
      x23::rpc_stream_logged(cp_, *c).server(&exec_to_coord_server_);
    }
  });
}

} // namespace hogd
