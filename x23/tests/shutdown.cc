#include <gtest/gtest.h>

#include <string>

#include "tests/xdrtest.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"

namespace x23 {

namespace {

struct Connection {
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;

  Connection(int nthreads) : pool_(nthreads), cp_(pool_) {}

  std::uint16_t start(std::uint16_t portno = 0)
  {
    auto lfd = x23::tcplisten(portno);
    if (!lfd) {
      perror("listen");
      exit(EXIT_FAILURE);
    }
    portno = get_port(local_addr(lfd));

    [](Connection *server, x23::UniqueFd lfd) -> x23::Detached {
      co_await server->cp_.pool_.bg();
      while (auto c = co_await server->cp_.accept(lfd))
        x23::rpc_stream_logged(server->cp_, *c).server(server);
    }(this, std::move(lfd));
    return portno;
  }

  using rpc_service = blank_rpc_v1;
  x23::Async<void> null() { co_return; }
  x23::Async<Data> echo(Data arg) { co_return arg; }
};
static_assert(is_blank_rpc_v1<Connection *, x23::Async>);

} // namespace

TEST(ShutdownTest, AfterRPC)
{
  Connection c(1);

  auto portno = c.start();

  x23::ThreadPool pool(1);
  x23::CoroPoll cp(pool);

  auto fd = cp.tcpconnect("localhost", std::to_string(portno))();
  if (!fd) {
    exit(-1);
  }

  // This forces the threadpool to wait until a connection is formed (i.e. after
  // the background work begins running) before attempting to shutdown the
  // coropoll.
  auto client = x23::rpc_stream_logged(cp, *fd).template client<blank_rpc_v1>();
  client.null()();
}
} // namespace x23
